/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/11/22.
//

#include <string.h>
#include <vector>

#include "common/log/log.h"
#include "common/io/io.h"
#include "net/mysql_communicator.h"
#include "event/session_event.h"
#include "sql/operator/string_list_physical_operator.h"

// https://dev.mysql.com/doc/dev/mysql-server/latest/group__group__cs__capabilities__flags.html
// the flags below are negotiate by handshake packet
const uint32_t CLIENT_PROTOCOL_41   = 512;
//const uint32_t CLIENT_INTERACTIVE   = 1024;  // This is an interactive client
const uint32_t CLIENT_TRANSACTIONS  = 8192;  // Client knows about transactions.
const uint32_t CLIENT_SESSION_TRACK = (1UL << 23); // Capable of handling server state change information
const uint32_t CLIENT_DEPRECATE_EOF = (1UL << 24); // Client no longer needs EOF_Packet and will use OK_Packet instead
const uint32_t CLIENT_OPTIONAL_RESULTSET_METADATA = (1UL << 25); // The client can handle optional metadata information in the resultset. 
// Support optional extension for query parameters into the COM_QUERY and COM_STMT_EXECUTE packets.
//const uint32_t CLIENT_QUERY_ATTRIBUTES = (1UL << 27); 

// https://dev.mysql.com/doc/dev/mysql-server/latest/group__group__cs__column__definition__flags.html
// Column Definition Flags
//const uint32_t NOT_NULL_FLAG  = 1;
//const uint32_t PRI_KEY_FLAG   = 2;
//const uint32_t UNIQUE_KEY_FLAG   = 4;
//const uint32_t MULTIPLE_KEY_FLAG = 8;
//const uint32_t NUM_FLAG          = 32768; // Field is num (for clients)
//const uint32_t PART_KEY_FLAG     = 16384; // Intern; Part of some key.

enum ResultSetMetaData
{
  RESULTSET_METADATA_NONE = 0,
  RESULTSET_METADATA_FULL = 1,
};


/**
     Column types for MySQL
*/
enum enum_field_types {
  MYSQL_TYPE_DECIMAL,
  MYSQL_TYPE_TINY,
  MYSQL_TYPE_SHORT,
  MYSQL_TYPE_LONG,
  MYSQL_TYPE_FLOAT,
  MYSQL_TYPE_DOUBLE,
  MYSQL_TYPE_NULL,
  MYSQL_TYPE_TIMESTAMP,
  MYSQL_TYPE_LONGLONG,
  MYSQL_TYPE_INT24,
  MYSQL_TYPE_DATE,
  MYSQL_TYPE_TIME,
  MYSQL_TYPE_DATETIME,
  MYSQL_TYPE_YEAR,
  MYSQL_TYPE_NEWDATE, /**< Internal to MySQL. Not used in protocol */
  MYSQL_TYPE_VARCHAR,
  MYSQL_TYPE_BIT,
  MYSQL_TYPE_TIMESTAMP2,
  MYSQL_TYPE_DATETIME2,   /**< Internal to MySQL. Not used in protocol */
  MYSQL_TYPE_TIME2,       /**< Internal to MySQL. Not used in protocol */
  MYSQL_TYPE_TYPED_ARRAY, /**< Used for replication only */
  MYSQL_TYPE_INVALID = 243,
  MYSQL_TYPE_BOOL = 244, /**< Currently just a placeholder */
  MYSQL_TYPE_JSON = 245,
  MYSQL_TYPE_NEWDECIMAL = 246,
  MYSQL_TYPE_ENUM = 247,
  MYSQL_TYPE_SET = 248,
  MYSQL_TYPE_TINY_BLOB = 249,
  MYSQL_TYPE_MEDIUM_BLOB = 250,
  MYSQL_TYPE_LONG_BLOB = 251,
  MYSQL_TYPE_BLOB = 252,
  MYSQL_TYPE_VAR_STRING = 253,
  MYSQL_TYPE_STRING = 254,
  MYSQL_TYPE_GEOMETRY = 255
};

// little endian
// We suppose our platform is little endian too
int store_int1(char *buf, int8_t value)
{
  *buf = value;
  return 1;
}

int store_int2(char *buf, int16_t value)
{
  memcpy(buf, &value, sizeof(value));
  return 2;
}

int store_int3(char *buf, int32_t value)
{
  memcpy(buf, &value, 3);
  return 3;
}

int store_int4(char *buf, int32_t value)
{
  memcpy(buf, &value, 4);
  return 4;
}

int store_int6(char *buf, int64_t value)
{
  memcpy(buf, &value, 6);
  return 6;
}

int store_int8(char *buf, int64_t value)
{
  memcpy(buf, &value, 8);
  return 8;
}

int store_lenenc_int(char *buf, uint64_t value)
{
  if (value < 251) {
    *buf = (int8_t)value;
    return 1;
  }

  if (value < (2UL << 16)) {
    *buf = 0xFC;
    memcpy(buf + 1, &value, 2);
    return 3;
  }

  if (value < (2UL << 24)) {
    *buf = 0xFD;
    memcpy(buf + 1, &value, 3);
    return 4;
  }

  *buf = 0xFE;
  memcpy(buf + 1, &value, 8);
  return 9;
}

int store_null_terminated_string(char *buf, const char *s)
{
  if (nullptr == s || s[0] == 0) {
    return 0;
  }

  const int len = strlen(s) + 1;
  memcpy(buf, s, len);
  return len;
}

int store_fix_length_string(char *buf, const char *s, int len)
{
  if (len == 0) {
    return 0;
  }

  memcpy(buf, s, len);
  return len;
}

int store_lenenc_string(char *buf, const char *s)
{
  int len = strlen(s);
  int pos = store_lenenc_int(buf, len);
  store_fix_length_string(buf + pos, s, len);
  return pos + len;
}

/**
 * 每个包都有一个包头
 * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_basic_packets.html
 * https://mariadb.com/kb/en/0-packet/
 */
struct PacketHeader {
  int32_t payload_length:24; //! 当前packet的除掉头的长度
  int8_t  sequence_id = 0;   //! 当前packet在当前处理过程中是第几个包
};

class BasePacket
{
public: 
  PacketHeader packet_header;

  BasePacket(int8_t sequence = 0)
  {
    packet_header.sequence_id = sequence;
  }

  virtual ~BasePacket() = default;
  virtual RC encode(uint32_t capabilities, std::vector<char> &net_packet) const = 0;
};

/**
 * 握手包
 * 先由服务端发送到客户端
 * 这个包会交互capability与用户名密码
 * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets_protocol_handshake_v10.html
 */
struct HandshakeV10 : public BasePacket
{
  int8_t       protocol = 10;
  char         server_version[7] = "5.7.25";
  int32_t      thread_id = 21501807; // conn id
  char         auth_plugin_data_part_1[9] = "12345678"; // first 8 bytes of the plugin provided data (scramble) // and the filler 
  int16_t      capability_flags_1 = 0xF7DF; // The lower 2 bytes of the Capabilities Flags
  int8_t       character_set = 83;
  int16_t      status_flags = 0;
  int16_t      capability_flags_2 = 0x0000;
  int8_t       auth_plugin_data_len = 0;
  char         reserved[10] = {0};
  char         auth_plugin_data_part_2[13] = "bbbbbbbbbbbb";

  HandshakeV10(int8_t sequence = 0) : BasePacket(sequence)
  {}
  virtual ~HandshakeV10() = default;

  /**
   * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets_protocol_handshake_v10.html
   */
  virtual RC encode(uint32_t capabilities, std::vector<char> &net_packet) const override
  {
    net_packet.resize(100);

    char *buf = net_packet.data();
    int pos = 0;
    pos += 3;

    pos += store_int1(buf + pos, packet_header.sequence_id);
    pos += store_int1(buf + pos, protocol);

    pos += store_null_terminated_string(buf + pos, server_version);
    pos += store_int4(buf + pos, thread_id);
    pos += store_null_terminated_string(buf + pos, auth_plugin_data_part_1);
    pos += store_int2(buf + pos, capability_flags_1);
    pos += store_int1(buf + pos, character_set);
    pos += store_int2(buf + pos, status_flags);
    pos += store_int2(buf + pos, capability_flags_2);
    pos += store_int1(buf + pos, auth_plugin_data_len);
    pos += store_fix_length_string(buf + pos, reserved, 10);
    pos += store_null_terminated_string(buf + pos, auth_plugin_data_part_2);

    int payload_length = pos - 4;
    store_int3(buf, payload_length);
    net_packet.resize(pos);
    LOG_TRACE("encode handshake packet with payload length=%d", payload_length);
    
    return RC::SUCCESS;
  }
};

struct OkPacket : public BasePacket
{
  int8_t       header = 0; // 0x00 for ok and 0xFE for EOF
  int32_t      affected_rows = 0;
  int32_t      last_insert_id = 0;
  int16_t      status_flags = 0x22;
  int16_t      warnings = 0;
  std::string  info; // human readable status information

  OkPacket(int8_t sequence = 0) : BasePacket(sequence)
  {}
  virtual ~OkPacket() = default;

  /**
   * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_basic_ok_packet.html
   */
  virtual RC encode(uint32_t capabilities, std::vector<char> &net_packet) const override
  {
    net_packet.resize(100);
    char *buf = net_packet.data();
    int pos = 0;

    pos += 3;
    pos += store_int1(buf + pos, packet_header.sequence_id);
    pos += store_int1(buf + pos, header);
    pos += store_lenenc_int(buf + pos, affected_rows);
    pos += store_lenenc_int(buf + pos, last_insert_id);

    if (capabilities & CLIENT_PROTOCOL_41) {
      pos += store_int2(buf + pos, status_flags);
      pos += store_int2(buf + pos, warnings);
    } else if (capabilities & CLIENT_TRANSACTIONS) {
      pos += store_int2(buf + pos, status_flags);
    }

    if (capabilities & CLIENT_SESSION_TRACK) {
      pos += store_lenenc_string(buf + pos, info.c_str());
    } else {
      pos += store_fix_length_string(buf + pos, info.c_str(), info.length());
    }

    int32_t payload_length = pos - 4;
    LOG_TRACE("encode ok packet with length=%d", payload_length);
    store_int3(buf, payload_length);
    net_packet.resize(pos);
    return RC::SUCCESS;
  }
};

struct EofPacket : public BasePacket
{
  int8_t       header = 0xFE;
  int16_t      warnings = 0;
  int16_t      status_flags = 0x22;

  EofPacket(int8_t sequence = 0) : BasePacket(sequence)
  {}
  virtual ~EofPacket() = default;

  /**
   * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_basic_err_packet.html
   */
  virtual RC encode(uint32_t capabilities, std::vector<char> &net_packet) const override
  {
    net_packet.resize(10);
    char *buf = net_packet.data();
    int pos = 0;

    pos += 3;
    store_int1(buf + pos, packet_header.sequence_id);
    pos += 1;
    store_int1(buf + pos, header);
    pos += 1;

    if (capabilities & CLIENT_PROTOCOL_41) {
      store_int2(buf + pos, warnings);
      pos += 2;
      store_int2(buf + pos, status_flags);
      pos += 2;
    }

    int payload_length = pos - 4;
    store_int3(buf, payload_length);
    net_packet.resize(pos);

    return RC::SUCCESS;
  }
};

struct ErrPacket : public BasePacket
{
  int8_t       header = 0xFF;
  int16_t      error_code = 0;
  char         sql_state_marker[1] = {'#'};
  std::string  sql_state{"HY000"};
  std::string  error_message;

  ErrPacket(int8_t sequence = 0) : BasePacket(sequence)
  {}
  virtual ~ErrPacket() = default;

  /**
   * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_basic_eof_packet.html
   */
  virtual RC encode(uint32_t capabilities, std::vector<char> &net_packet) const override
  {
    net_packet.resize(1000);
    char *buf = net_packet.data();
    int pos = 0;

    pos += 3;

    store_int1(buf + pos, packet_header.sequence_id);
    pos += 1;
    store_int1(buf + pos, header);
    pos += 1;
    store_int2(buf + pos, error_code);
    pos += 2;
    if (capabilities & CLIENT_PROTOCOL_41) {
      pos += store_fix_length_string(buf + pos, sql_state_marker, 1);
      pos += store_fix_length_string(buf + pos, sql_state.c_str(), 5);
    }

    pos += store_fix_length_string(buf + pos, error_message.c_str(), error_message.length());

    int payload_length = pos - 4;
    store_int3(buf, payload_length);
    net_packet.resize(pos);
    return RC::SUCCESS;
  }
};

// https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_command_phase.html
// https://mariadb.com/kb/en/2-text-protocol/
struct QueryPacket
{
  PacketHeader packet_header;
  int8_t       command; // 0x03: COM_QUERY
  std::string  query;   // the text of the SQL query to execute
};

/**
 * decode query packet
 * packet_header is not included in net_packet
 * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_query.html
 */
RC decode_query_packet(std::vector<char> &net_packet, QueryPacket &query_packet)
{
  // query field is a null terminated string
  query_packet.query.assign(net_packet.data() + 1, net_packet.size() - 1);
  query_packet.query.append(1, ';');
  return RC::SUCCESS;
}

RC create_version_comment_sql_result(SqlResult *&sql_result)
{
  TupleSchema tuple_schema;
  TupleCellSpec cell_spec("", "", "@@version_comment");
  tuple_schema.append_cell(cell_spec);

  sql_result = new SqlResult;
  sql_result->set_return_code(RC::SUCCESS);
  sql_result->set_tuple_schema(tuple_schema);

  const char *version_comments = "MiniOB";

  StringListPhysicalOperator *oper = new StringListPhysicalOperator();
  oper->append(version_comments);
  sql_result->set_operator(std::unique_ptr<PhysicalOperator>(oper));
  return RC::SUCCESS;
}

RC MysqlCommunicator::init(int fd, Session *session, const std::string &addr)
{
  // https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase.html
  // 按照协议描述，服务端在连接建立后需要先向客户端发送握手信息
  RC rc = Communicator::init(fd, session, addr);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to init communicator: %s", strrc(rc));
    return rc;
  }

  HandshakeV10 handshake_packet;
  rc = send_packet(handshake_packet);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to send handshake packet to client. addr=%s, error=%s", addr.c_str(), strerror(errno));
    return rc;
  }

  return rc;
}

RC MysqlCommunicator::handle_version_comment(bool &need_disconnect)
{
  SqlResult *sql_result = nullptr;
  RC rc = create_version_comment_sql_result(sql_result);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to handle version comment. rc=%s", strrc(rc));
    return rc;
  }

  SessionEvent session_event(this);
  session_event.set_sql_result(sql_result);
  rc = write_result(&session_event, need_disconnect);
  return rc;
}

RC MysqlCommunicator::read_event(SessionEvent *&event)
{
  RC rc = RC::SUCCESS;
  PacketHeader packet_header;
  int ret = common::readn(fd_, &packet_header, sizeof(packet_header));
  if (ret != 0) {
    LOG_WARN("failed to read packet header. length=%d, addr=%s. error=%s", sizeof(packet_header), addr_.c_str(), strerror(errno));
    return RC::IOERR;
  }

  LOG_TRACE("read packet header. length=%d, sequence_id=%d", sizeof(packet_header), packet_header.sequence_id);
  sequence_id_ = packet_header.sequence_id + 1;
  
  std::vector<char> buf(packet_header.payload_length);
  ret = common::readn(fd_, buf.data(), packet_header.payload_length);
  if (ret != 0) {
    LOG_WARN("failed to read packet payload. length=%d, addr=%s, error=%s",
             packet_header.payload_length, addr_.c_str(), strerror(errno));
    return RC::IOERR;
  }

  LOG_TRACE("read packet payload length=%d", packet_header.payload_length);
  
  event = nullptr;
  if (!authed_) {
    uint32_t client_flag = *(uint32_t*)buf.data(); // TODO should use decode (little endian as default)
    LOG_INFO("client handshake response with capabilities flag=%d", client_flag);
    client_capabilities_flag_ = client_flag;
    // send ok packet and return
    OkPacket ok_packet;
    ok_packet.packet_header.sequence_id = sequence_id_;
    rc = send_packet(ok_packet);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to send ok packet while auth");
    }
    authed_ = true;
    LOG_INFO("client authed. addr=%s. rc=%s", addr_.c_str(), strrc(rc));
    return rc;
  }

  int8_t command_type = buf[0];
  LOG_TRACE("recv command from client =%d", command_type);
  
  if (command_type == 0x03) {  // COM_QUERY
    QueryPacket query_packet;
    rc = decode_query_packet(buf, query_packet);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to decode query packet. packet length=%ld, addr=%s, error=%s", buf.size(), addr(), strrc(rc));
      return rc;
    }

    LOG_TRACE("query command: %s", query_packet.query.c_str());
    if (query_packet.query.find("select @@version_comment") != std::string::npos) {
      bool need_disconnect;
      return handle_version_comment(need_disconnect);
    }

    event = new SessionEvent(this);
    event->set_query(query_packet.query);
  } else {
    OkPacket ok_packet(sequence_id_);
    rc = send_packet(ok_packet);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to send ok packet. command=%d, addr=%s, error=%s", command_type, addr(), strrc(rc));
      return rc;
    }
  }
  return rc;
}

RC MysqlCommunicator::write_state(SessionEvent *event, bool &need_disconnect)
{
  SqlResult *sql_result = event->sql_result();
  
  const int buf_size = 2048;
  char *buf = new char[buf_size];
  const std::string &state_string = sql_result->state_string();
  if (state_string.empty()) {
    const char *result = RC::SUCCESS == sql_result->return_code() ? "SUCCESS" : "FAILURE";
    snprintf(buf, buf_size, "%s", result);
  } else {
    snprintf(buf, buf_size, "%s > %s", strrc(sql_result->return_code()), state_string.c_str());
  }

  RC rc = RC::SUCCESS;
  if (sql_result->return_code() == RC::SUCCESS) {

    OkPacket ok_packet;
    ok_packet.packet_header.sequence_id = sequence_id_++;
    ok_packet.info.assign(buf);
    rc = send_packet(ok_packet);
  } else {
    ErrPacket err_packet;
    err_packet.packet_header.sequence_id = sequence_id_++;
    err_packet.error_code = static_cast<int>(sql_result->return_code());
    err_packet.error_message = buf;
    rc = send_packet(err_packet);
  }
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to send ok packet to client. addr=%s, error=%s", addr(), strrc(rc));
    need_disconnect = true;
  } else {
    need_disconnect = false;
  }

  delete[] buf;
  return rc;
}

RC MysqlCommunicator::write_result(SessionEvent *event, bool &need_disconnect)
{
  RC rc = RC::SUCCESS;
  
  need_disconnect = true;
  SqlResult *sql_result = event->sql_result();
  if (nullptr == sql_result) {
    const char *response = event->get_response();
    int len = event->get_response_len();

    OkPacket ok_packet;// TODO if error occurs, we should send an error packet to client
    ok_packet.info.assign(response, len);
    rc = send_packet(ok_packet);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to send ok packet to client. addr=%s, rc=%s", addr(), strrc(rc));
      return rc;
    }

    need_disconnect = false;
  } else {
    if (RC::SUCCESS != sql_result->return_code() || !sql_result->has_operator()) {
      return write_state(event, need_disconnect);      
    }

    // send result set
    // https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_query_response_text_resultset.html
    RC rc = sql_result->open();
    if (rc != RC::SUCCESS) {
      sql_result->set_return_code(rc);
      return write_state(event, need_disconnect);
    }

    // send metadata : Column Definition
    rc = send_column_definition(sql_result, need_disconnect);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    rc = send_result_rows(sql_result, need_disconnect);
  }
  
  return rc;
}

RC MysqlCommunicator::send_packet(const BasePacket &packet)
{
  std::vector<char> net_packet;
  RC rc = packet.encode(client_capabilities_flag_, net_packet);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to encode ok packet. rc=%s", strrc(rc));
    return rc;
  }

  int ret = common::writen(fd_, net_packet.data(), net_packet.size());
  if (ret != 0) {
    LOG_WARN("failed to send packet to client. addr=%s, error=%s", addr(), strerror(errno));
    return RC::IOERR;
  }

  LOG_TRACE("send ok packet success. packet length=%d", net_packet.size());
  return rc;
}

/**
 * 发送列定义信息
 *  https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_query_response_text_resultset.html
 *  https://mariadb.com/kb/en/result-set-packets/#column-definition-packet
 *
 * 先发送当前有多少个列
 * 然后发送N个包，告诉客户端每个列的信息
 */
RC MysqlCommunicator::send_column_definition(SqlResult *sql_result, bool &need_disconnect)
{
  RC rc = RC::SUCCESS;
  const TupleSchema &tuple_schema = sql_result->tuple_schema();
  const int cell_num = tuple_schema.cell_num();

  std::vector<char> net_packet;
  net_packet.resize(1024);
  char *buf = net_packet.data();
  int pos = 0;

  pos += 3;
  store_int1(buf + pos, sequence_id_++);
  pos += 1;

  if (client_capabilities_flag_ & CLIENT_OPTIONAL_RESULTSET_METADATA) {
    store_int1(buf + pos, static_cast<int>(ResultSetMetaData::RESULTSET_METADATA_FULL));
    pos += 1;
    LOG_TRACE("client with optional resultset metadata");
  } else {
    LOG_TRACE("client without optional resultset metadata");
  }

  pos += store_lenenc_int(buf + pos, cell_num);

  int payload_length = pos - 4;
  store_int3(buf, payload_length);
  net_packet.resize(pos);

  int ret = common::writen(fd_, net_packet.data(), net_packet.size());
  if (ret != 0){
    LOG_WARN("failed to send column count to client. addr=%s, error=%s", addr(), strerror(errno));
    need_disconnect = true;
    return RC::IOERR;
  }

  for (int i = 0; i < cell_num; i++) {
    net_packet.resize(1024);
    buf = net_packet.data();
    pos = 0;

    pos += 3;
    store_int1(buf + pos, sequence_id_++);
    pos += 1;

    const TupleCellSpec &spec = tuple_schema.cell_at(i);
    const char *catalog = "def"; // The catalog used. Currently always "def"
    const char *schema = "sys"; // schema name
    const char *table  = spec.table_name();
    const char *org_table = spec.table_name();
    const char *name = spec.alias();
    //const char *org_name = spec.field_name();
    const char *org_name = spec.alias();
    int fixed_len_fields = 0x0c;
    int character_set = 33;
    int column_length = 16384;
    int type = MYSQL_TYPE_VAR_STRING;
    int16_t flags = 0;
    int8_t decimals = 0x1f;

    pos += store_lenenc_string(buf + pos, catalog);
    pos += store_lenenc_string(buf + pos, schema);
    pos += store_lenenc_string(buf + pos, table);
    pos += store_lenenc_string(buf + pos, org_table);
    pos += store_lenenc_string(buf + pos, name);
    pos += store_lenenc_string(buf + pos, org_name);
    pos += store_lenenc_int(buf + pos, fixed_len_fields);
    store_int2(buf + pos, character_set);
    pos += 2;
    store_int4(buf + pos, column_length);
    pos += 4;
    store_int1(buf + pos, type);
    pos += 1;
    store_int2(buf + pos, flags);
    pos += 2;
    store_int1(buf + pos, decimals);
    pos += 1;
    store_int2(buf + pos, 0); // 按照mariadb的文档描述，最后还有一个unused字段int<2>，不过mysql的文档没有给出这样的描述 
    pos += 2;

    payload_length = pos - 4;
    store_int3(buf, payload_length);
    net_packet.resize(pos);

    ret = common::writen(fd_, net_packet.data(), net_packet.size());
    if (ret != 0) {
      LOG_WARN("failed to write column definition to client. addr=%s, error=%s", addr(), strerror(errno));
      need_disconnect = true;
      return RC::IOERR;
    }
  }

  if (!(client_capabilities_flag_ & CLIENT_DEPRECATE_EOF)) {
    EofPacket eof_packet;
    eof_packet.packet_header.sequence_id = sequence_id_++;
    eof_packet.status_flags = 0x02;
    rc = send_packet(eof_packet);
    if (rc != RC::SUCCESS) {
      need_disconnect = true;
      LOG_WARN("failed to send eof packet to client. addr=%s, error=%s", addr(), strerror(errno));
    }
  } else {
    LOG_TRACE("client use CLIENT_DEPRECATE_EOF");
  }

  LOG_TRACE("send column definition to client done");
  need_disconnect = false;
  return RC::SUCCESS;
}

/**
 * 发送每行数据
 * 一行一个包
 */
RC MysqlCommunicator::send_result_rows(SqlResult *sql_result, bool &need_disconnect)
{
  RC rc = RC::SUCCESS;
  std::vector<char> packet;
  packet.resize(4 * 1024 * 1024); // TODO warning: length cannot be fix
  
  Tuple *tuple = nullptr;
  while (RC::SUCCESS == (rc = sql_result->next_tuple(tuple))) {
    // https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_query_response_text_resultset.html
    // https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_query_response_text_resultset_row.html
    // note: if some field is null, send a 0xFB
    char *buf = packet.data();
    int pos = 0;

    pos += 3;
    store_int1(buf + pos, sequence_id_++);
    pos += 1;

    assert(tuple != nullptr);
    const int cell_num = tuple->cell_num();
    TupleCell tuple_cell;
    for (int i = 0; i < cell_num; i++) {
      rc = tuple->cell_at(i, tuple_cell);
      if (rc != RC::SUCCESS) {
        sql_result->set_return_code(rc);
        break; // TODO send error packet
      }

      std::stringstream ss;
      tuple_cell.to_string(ss);
      pos += store_lenenc_string(buf + pos, ss.str().c_str());    
    }

    int payload_length = pos - 4;
    store_int3(buf, payload_length);
    int ret = common::writen(fd_, buf, pos);
    if (ret != 0) {
      LOG_WARN("failed to send row packet to client. addr=%s, error=%s", addr(), strerror(errno));
      need_disconnect = true;
      return RC::IOERR;
    }
  }

  // 所有行发送完成后，发送一个EOF或OK包
  if (client_capabilities_flag_ & CLIENT_DEPRECATE_EOF) {
    LOG_TRACE("client has CLIENT_DEPRECATE_EOF, send ok packet");
    OkPacket ok_packet;
    ok_packet.packet_header.sequence_id = sequence_id_++;
    rc = send_packet(ok_packet);
  } else {
    EofPacket eof_packet;
    eof_packet.packet_header.sequence_id = sequence_id_++;
    rc = send_packet(eof_packet);
  }

  LOG_TRACE("send rows to client done");
  need_disconnect = false;
  return rc;
}

/*
 * @Author: Wang
 * @Date: 2025-06-04 10:35:05
 * @LastEditors: Please set LastEditors
 * @LastEditTime: 2025-09-26 16:02:03
 * @Description: UserService with prepared statements and LOG_INFO
 */

 #include "UserService.h"
 #include "../pool/sqlconnRAII.h"
 #include <mysql/mysql.h>
 #include <cstring>
 
 // UserExists
 bool UserService::UserExists(const std::string& username) {
     MYSQL* sql;
     SqlConnRAII conn(&sql, SqlConnPool::Instance());
 
     MYSQL_STMT* stmt = mysql_stmt_init(sql);
     const char* q = "SELECT 1 FROM user WHERE username=? LIMIT 1";
     if (mysql_stmt_prepare(stmt, q, strlen(q))) {
         LOG_INFO("MySQL prepare error: %s", mysql_error(sql));
         mysql_stmt_close(stmt);
         return false;
     }
 
     MYSQL_BIND bind_param[1]{};
     bind_param[0].buffer_type = MYSQL_TYPE_STRING;
     bind_param[0].buffer = (void*)username.c_str();
     bind_param[0].buffer_length = username.size();
     mysql_stmt_bind_param(stmt, bind_param);
 
     if (mysql_stmt_execute(stmt)) {
         LOG_INFO("MySQL execute error: %s", mysql_error(sql));
         mysql_stmt_close(stmt);
         return false;
     }
 
     int dummy = 0;
     MYSQL_BIND bind_result[1]{};
     bind_result[0].buffer_type = MYSQL_TYPE_LONG;
     bind_result[0].buffer = &dummy;
     mysql_stmt_bind_result(stmt, bind_result);
 
     bool exists = (mysql_stmt_fetch(stmt) == 0);
 
     mysql_stmt_close(stmt);
     return exists;
 }
 
 // GetUserPasswordHash
 bool UserService::GetUserPasswordHash(const std::string& username, std::string& hash, int& userID) {
     MYSQL* sql;
     SqlConnRAII conn(&sql, SqlConnPool::Instance());
 
     MYSQL_STMT* stmt = mysql_stmt_init(sql);
     const char* q = "SELECT id, password FROM user WHERE username=? LIMIT 1";
     if (mysql_stmt_prepare(stmt, q, strlen(q))) {
         LOG_INFO("MySQL prepare error: %s", mysql_error(sql));
         mysql_stmt_close(stmt);
         return false;
     }
 
     MYSQL_BIND bind_param[1]{};
     bind_param[0].buffer_type = MYSQL_TYPE_STRING;
     bind_param[0].buffer = (void*)username.c_str();
     bind_param[0].buffer_length = username.size();
     mysql_stmt_bind_param(stmt, bind_param);
 
     if (mysql_stmt_execute(stmt)) {
         LOG_INFO("MySQL execute error: %s", mysql_error(sql));
         mysql_stmt_close(stmt);
         return false;
     }
 
     int id;
     char pwd[512];
     unsigned long pwd_len = 0;
 
     MYSQL_BIND bind_result[2]{};
     bind_result[0].buffer_type = MYSQL_TYPE_LONG;
     bind_result[0].buffer = &id;
 
     bind_result[1].buffer_type = MYSQL_TYPE_STRING;
     bind_result[1].buffer = pwd;
     bind_result[1].buffer_length = sizeof(pwd);
     bind_result[1].length = &pwd_len;
 
     mysql_stmt_bind_result(stmt, bind_result);
 
     if (mysql_stmt_fetch(stmt) != 0) {
         // 没有找到
         mysql_stmt_close(stmt);
         return false;
     }
 
     userID = id;
     hash.assign(pwd, pwd_len);
 
     mysql_stmt_close(stmt);
     return true;
 }
 
 // InsertNewUser
 bool UserService::InsertNewUser(const std::string& username, const std::string& hash, int& userID) {
     MYSQL* sql;
     SqlConnRAII conn(&sql, SqlConnPool::Instance());
 
     MYSQL_STMT* stmt = mysql_stmt_init(sql);
     const char* q = "INSERT INTO user(username, password) VALUES(?, ?)";
     if (mysql_stmt_prepare(stmt, q, strlen(q))) {
         LOG_INFO("MySQL prepare error: %s", mysql_error(sql));
         mysql_stmt_close(stmt);
         return false;
     }
 
     MYSQL_BIND bind_param[2]{};
     bind_param[0].buffer_type = MYSQL_TYPE_STRING;
     bind_param[0].buffer = (void*)username.c_str();
     bind_param[0].buffer_length = username.size();
 
     bind_param[1].buffer_type = MYSQL_TYPE_STRING;
     bind_param[1].buffer = (void*)hash.c_str();
     bind_param[1].buffer_length = hash.size();
 
     mysql_stmt_bind_param(stmt, bind_param);
 
     if (mysql_stmt_execute(stmt)) {
         LOG_INFO("MySQL insert error: %s", mysql_error(sql));
         mysql_stmt_close(stmt);
         return false;
     }
 
     userID = static_cast<int>(mysql_insert_id(sql));
     mysql_stmt_close(stmt);
     return true;
 }
 
/*
 * @Author: Wang
 * @Date: 2025-06-04 10:54:45
 * @LastEditors: Please set LastEditors
 * @LastEditTime: 2025-09-29 17:06:23
 * @Description: 请填写简介
 */
#include "uploadservice.h"
#include <fstream>
#include <filesystem>
#include <mysql/mysql.h>
#include "../pool/sqlconnpool.h"
#include "../processing/uploaded_file.h"

std::shared_ptr<sw::redis::Redis> UploadService::redis_ = nullptr;
void UploadService::InitRedis(const std::shared_ptr<sw::redis::Redis> &redis)
{
    redis_ = redis;
}

// 文件名安全处理
std::string SanitizeFilename(const std::string &filename)
{
    std::string safe_name;
    for (char c : filename)
    {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' ')
        {
            safe_name += c;
        }
    }

    // 去除首尾空格
    size_t start = safe_name.find_first_not_of(" ");
    size_t end = safe_name.find_last_not_of(" ");
    if (start != std::string::npos && end != std::string::npos)
    {
        safe_name = safe_name.substr(start, end - start + 1);
    }

    // 限制文件名长度
    if (safe_name.length() > 255)
    {
        safe_name = safe_name.substr(0, 255);
    }

    // 如果文件名为空，生成默认文件名
    if (safe_name.empty())
    {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch())
                             .count();
        safe_name = "file_" + std::to_string(timestamp) + ".dat";
    }

    return safe_name;
}

// 创建目录（改进版）
bool CreateDirectoryIfNotExists(const std::string &path)
{
    try
    {
        std::filesystem::path dir(path);
        if (!std::filesystem::exists(dir))
        {
            bool created = std::filesystem::create_directories(dir);
            if (created)
            {
                LOG_INFO("目录创建成功：%s",path.c_str());
            }
            return created;
        }
        return true;
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        LOG_INFO("目录创建错误：%s",e.what());
        return false;
    }
    catch (...)
    {
        LOG_DEBUG("未知目录创建错误");
        return false;
    }
}

bool UploadService::SaveUploadedFile(const UploadedFile &file, int user_id)
{
    LOG_INFO("开始本地磁盘写入");

    // 1. 安全检查
    if (file.filename.empty() || file.content.empty())
    {
        LOG_INFO("错误：文件名或内容为空");
        return false;
    }

    // 2. 防止路径遍历攻击
    std::string safe_filename = SanitizeFilename(file.filename);
    if (safe_filename.empty())
    {
        LOG_INFO("错误：文件名不安全");
        return false;
    }

    // 3. 创建目录（如果不存在）
    std::string directory = "./resources/images/";
    if (!CreateDirectoryIfNotExists(directory))
    {
        LOG_INFO("错误: 无法创建目录");
        return false;
    }

    std::string filepath = directory + safe_filename;
    bool fileWritten = false;          // 标记文件是否已写入
    bool dbTransactionStarted = false; // 标记数据库事务是否开始
    MYSQL *sql = nullptr;

    try
    {
        // 4. 改进的文件写入
        std::ofstream ofs(filepath, std::ios::binary);
        if (!ofs)
        {
            LOG_INFO("错误: 无法打开文件:%s ",filepath.c_str() );
            return false;
        }

        ofs.write(file.content.data(), file.content.size());
        if (!ofs.good())
        {
            LOG_INFO("错误: 文件写入失败");
            ofs.close();
            std::remove(filepath.c_str());
            return false;
        }
        ofs.close();
        fileWritten = true; // 标记文件写入成功
        LOG_INFO("文件写入成功: %s",filepath.c_str());

        // 5. 数据库操作 - 使用事务确保原子性
        SqlConnRAII(&sql, SqlConnPool::Instance());
        if (!sql)
        {
            LOG_INFO("错误: 无法获取数据库连接");
            throw std::runtime_error("数据库连接失败");
        }

        // 开始事务
        if (mysql_autocommit(sql, 0) != 0)
        {
            LOG_INFO("错误: 无法开始事务");
            throw std::runtime_error("事务开始失败");
        }
        dbTransactionStarted = true;
        LOG_INFO("数据库事务开始");

        // 使用预处理语句防止SQL注入
        std::string query = "INSERT INTO uploaded_files "
                            "(original_filename, stored_filename, file_path, file_size, upload_time, file_type, uploader_id) "
                            "VALUES (?, ?, ?, ?, NOW(), ?, ?)";

        MYSQL_STMT *stmt = mysql_stmt_init(sql);
        if (!stmt)
        {
            LOG_INFO("错误: 无法初始化预处理语句");
            throw std::runtime_error("预处理语句初始化失败");
        }

        if (mysql_stmt_prepare(stmt, query.c_str(), query.length()) != 0)
        {
            LOG_INFO("错误: 预处理语句准备失败:%s",mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            throw std::runtime_error("预处理语句准备失败");
        }

        // 绑定参数
        MYSQL_BIND bind[6];
        memset(bind, 0, sizeof(bind));

        std::string original_filename = file.filename;
        std::string stored_filename = safe_filename;
        std::string file_path = filepath;
        unsigned long file_size = file.content.size(); // 使用unsigned long避免负数
        std::string file_type = file.contentType;

        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (char *)original_filename.c_str();
        bind[0].buffer_length = original_filename.length();

        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (char *)stored_filename.c_str();
        bind[1].buffer_length = stored_filename.length();

        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = (char *)file_path.c_str();
        bind[2].buffer_length = file_path.length();

        bind[3].buffer_type = MYSQL_TYPE_LONG;
        bind[3].buffer = &file_size;
        bind[3].is_unsigned = 1; // 文件大小不能为负数

        bind[4].buffer_type = MYSQL_TYPE_STRING;
        bind[4].buffer = (char *)file_type.c_str();
        bind[4].buffer_length = file_type.length();

        bind[5].buffer_type = MYSQL_TYPE_LONG;
        bind[5].buffer = &user_id;

        if (mysql_stmt_bind_param(stmt, bind) != 0)
        {
            LOG_INFO("错误: 参数绑定失败: %s",mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            throw std::runtime_error("参数绑定失败");
        }

        // 执行插入
        if (mysql_stmt_execute(stmt) != 0)
        {
            LOG_INFO("错误: 数据库插入失败: %s",mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            throw std::runtime_error("数据库插入失败");
        }

        // 提交事务
        if (mysql_commit(sql) != 0)
        {
            LOG_INFO("错误: 事务提交失败");
            throw std::runtime_error("事务提交失败");
        }

        mysql_autocommit(sql, 1); // 恢复自动提交
        mysql_stmt_close(stmt);
        dbTransactionStarted = false; // 事务已完成

        LOG_INFO("数据库记录插入成功" );
        LOG_INFO("文件上传完成:%s,%s",file.filename.c_str(),safe_filename.c_str());

        // 6. Redis 缓存处理 — 放在 COMMIT 成功之后
        try {
            // 简单策略：只删除缓存，下次查询自动回填完整列表
            if (redis_) {
                redis_->del("user:" + std::to_string(user_id) + ":filelist");
            }
        } catch (const std::exception &ex) {
            LOG_INFO("Redis 缓存更新失败: %s", ex.what());
        }

        return true;
    }
    catch (const std::exception &e)
    {
        LOG_INFO("异常发生: %s",e.what());


        // 回滚操作
        if (dbTransactionStarted && sql)
        {
            LOG_INFO("执行数据库回滚");
            mysql_rollback(sql);
            mysql_autocommit(sql, 1); // 恢复自动提交
        }

        // 如果文件已写入但数据库操作失败，删除文件
        if (fileWritten)
        {
            LOG_INFO( "清理已写入的文件: %s",filepath.c_str() );
            std::remove(filepath.c_str());
        }

        return false;
    }
    catch (...)
    {
        LOG_INFO("未知异常发生" );

        // 回滚操作
        if (dbTransactionStarted && sql)
        {
            LOG_INFO("执行数据库回滚");
            mysql_rollback(sql);
            mysql_autocommit(sql, 1);
        }

        // 清理文件
        if (fileWritten)
        {
            LOG_INFO("清理已写入的文件: %s",filepath.c_str());
            std::remove(filepath.c_str());
        }

        return false;
    }
}

bool UploadService::DeleteFile(const std::string &filename, int user_id)
{
    // 先删除文件
    std::string filepath = "./resources/images/" + filename;
    if (!std::filesystem::exists(filepath))
        return false;
    if (!std::filesystem::remove(filepath))
        return false;

    // 再删除数据库记录
    MYSQL *sql;
    SqlConnRAII conn(&sql, SqlConnPool::Instance());

    MYSQL_STMT *stmt = mysql_stmt_init(sql);
    const char *q = "DELETE FROM uploaded_files WHERE stored_filename=? AND uploader_id=?";
    if (mysql_stmt_prepare(stmt, q, strlen(q)))
    {
        LOG_INFO("MySQL prepare error: %s", mysql_error(sql));
        mysql_stmt_close(stmt);
        return false;
    }

    MYSQL_BIND bind[2]{};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)filename.c_str();
    bind[0].buffer_length = filename.size();

    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = (void *)&user_id;

    mysql_stmt_bind_param(stmt, bind);

    if (mysql_stmt_execute(stmt))
    {
        LOG_INFO("MySQL delete error: %s", mysql_error(sql));
        mysql_stmt_close(stmt);
        return false;
    }

    mysql_stmt_close(stmt);
    return true;
}

std::vector<UploadedFileInfo> UploadService::QueryAllFiles(int userId)
{
    std::vector<UploadedFileInfo> result;
    std::string redisKey = "user:" + std::to_string(userId) + ":filelist";

    // 1. 先查 Redis 缓存
    if (redis_) {
        auto jsonStr = redis_->get(redisKey);
        if (jsonStr) {
            try {
                nlohmann::json j = nlohmann::json::parse(*jsonStr);
                for (auto &item : j) {
                    UploadedFileInfo info;
                    info.original_filename = item["original_filename"].get<std::string>();
                    info.stored_filename   = item["stored_filename"].get<std::string>();
                    info.file_path         = item["file_path"].get<std::string>();
                    info.file_size         = item["file_size"].get<int>();
                    info.upload_time       = item["upload_time"].get<std::string>();
                    info.file_type         = item["file_type"].get<std::string>();
                    info.uploader_id       = item["uploader_id"].get<int>();
                    result.push_back(info);
                }
                return result; // 缓存命中直接返回
            } catch (const std::exception &ex) {
                LOG_ERROR("Redis 缓存解析失败: %s", ex.what());
                // 继续走数据库
            }
        }
    }

    // 2. 未命中缓存 → 查 MySQL
    MYSQL *sql;
    SqlConnRAII(&sql, SqlConnPool::Instance());

    const char *query =
        "SELECT original_filename, stored_filename, file_path, file_size, "
        "upload_time, file_type, uploader_id FROM uploaded_files "
        "WHERE uploader_id = ? ORDER BY upload_time DESC";

    MYSQL_STMT *stmt = mysql_stmt_init(sql);
    if (!stmt || mysql_stmt_prepare(stmt, query, strlen(query)) != 0) {
        LOG_ERROR("MySQL 预处理失败: %s", mysql_error(sql));
        if (stmt) mysql_stmt_close(stmt);
        return result;
    }

    MYSQL_BIND bind_param{};
    bind_param.buffer_type = MYSQL_TYPE_LONG;
    bind_param.buffer = (char *)&userId;

    if (mysql_stmt_bind_param(stmt, &bind_param) != 0) {
        LOG_ERROR("MySQL 参数绑定失败: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        LOG_ERROR("MySQL 执行失败: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }

    MYSQL_RES *prepare_meta_result = mysql_stmt_result_metadata(stmt);
    if (!prepare_meta_result) {
        LOG_ERROR("MySQL 获取结果元数据失败: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }

    // 准备接收数据（注意缓冲区足够大）
    char orig[256], stored[256], path[256], time[64], type[64];
    int size = 0, uid = 0;

    MYSQL_BIND bind_result[7]{};
    bind_result[0].buffer_type = MYSQL_TYPE_STRING;
    bind_result[0].buffer = orig;
    bind_result[0].buffer_length = sizeof(orig);

    bind_result[1].buffer_type = MYSQL_TYPE_STRING;
    bind_result[1].buffer = stored;
    bind_result[1].buffer_length = sizeof(stored);

    bind_result[2].buffer_type = MYSQL_TYPE_STRING;
    bind_result[2].buffer = path;
    bind_result[2].buffer_length = sizeof(path);

    bind_result[3].buffer_type = MYSQL_TYPE_LONG;
    bind_result[3].buffer = (char *)&size;

    bind_result[4].buffer_type = MYSQL_TYPE_STRING;
    bind_result[4].buffer = time;
    bind_result[4].buffer_length = sizeof(time);

    bind_result[5].buffer_type = MYSQL_TYPE_STRING;
    bind_result[5].buffer = type;
    bind_result[5].buffer_length = sizeof(type);

    bind_result[6].buffer_type = MYSQL_TYPE_LONG;
    bind_result[6].buffer = (char *)&uid;

    mysql_stmt_bind_result(stmt, bind_result);

    while (mysql_stmt_fetch(stmt) == 0) {
        UploadedFileInfo info;
        info.original_filename = orig;
        info.stored_filename   = stored;
        info.file_path         = path;
        info.file_size         = size;
        info.upload_time       = time;
        info.file_type         = type;
        info.uploader_id       = uid;
        result.push_back(info);
    }

    mysql_free_result(prepare_meta_result);
    mysql_stmt_close(stmt);

    // 3. 回填 Redis 缓存
    if (redis_ && !result.empty()) {
        try {
            nlohmann::json j;
            for (auto &f : result) {
                j.push_back({
                    {"original_filename", f.original_filename},
                    {"stored_filename",   f.stored_filename},
                    {"file_path",         f.file_path},
                    {"file_size",         f.file_size},
                    {"upload_time",       f.upload_time},
                    {"file_type",         f.file_type},
                    {"uploader_id",       f.uploader_id}
                });
            }
            redis_->setex(redisKey, std::chrono::minutes(5), j.dump());
        } catch (const std::exception &ex) {
            LOG_ERROR("Redis 回填缓存失败: %s", ex.what());
        }
    }

    return result;
}

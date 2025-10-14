/*
 * @Author: Wang
 * @Date: 2025-06-11 10:17:16
 * @LastEditors: Please set LastEditors
 * @LastEditTime: 2025-09-29 15:41:12
 * @Description: 将cookie存储在redis中
 */

#pragma once
#include <memory>
#include <string>
#include <sw/redis++/redis++.h>

class RedisSessionManager
{
public:
    RedisSessionManager(int defaultTTL = 3600);

    std::string CreateSession(int userID, int ttl = -1);
    void RefreshSessionTTL(const std::string &token);
    std::optional<int> GetUserID(const std::string &token);
    void DeleteSession(const std::string &token);
    static void InitRedis(const std::shared_ptr<sw::redis::Redis>& redis);

private:
    static std::shared_ptr<sw::redis::Redis> redis_; // 全局共享
    int defaultTTL_;
    std::string GenerateToken(int length = 32);
};

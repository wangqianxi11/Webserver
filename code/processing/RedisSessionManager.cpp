/*
 * @Author: Wang
 * @Date: 2025-06-23 20:29:19
 * @LastEditors: Please set LastEditors
 * @LastEditTime: 2025-09-29 15:41:08
 * @Description: 请填写简介
 */


 #include "../processing/RedisSessionManager .h"
#include <random>
#include <chrono>
#include <mutex>

std::shared_ptr<sw::redis::Redis> RedisSessionManager::redis_ = nullptr;
static std::once_flag g_redis_init_once;

RedisSessionManager::RedisSessionManager(int defaultTTL)
: defaultTTL_(defaultTTL) {}

void RedisSessionManager::InitRedis(const std::shared_ptr<sw::redis::Redis>& redis) {
    std::call_once(g_redis_init_once, [&] {
        redis_ = redis;  // 仅初始化一次
    });
    // 如果多次调用，静默忽略或抛异常都行，这里选择忽略
}

std::string RedisSessionManager::CreateSession(int userID, int ttl) {
    auto r = redis_;
    if (!r) throw std::runtime_error("Redis not initialized");
    if (ttl <= 0) ttl = defaultTTL_;

    for (int attempt = 0; attempt < 5; ++attempt) {
        std::string token = GenerateToken();
        const std::string key = "session:" + token;
        const std::string val = std::to_string(userID);

        // 兼容老版 Redis++：set(key, val, ttl, UpdateType)
        if (r->set(key,
                   val,
                   std::chrono::seconds(ttl),
                   sw::redis::UpdateType::NOT_EXIST)) {  // ← NX
            return token;
        }
        // 碰撞极小，重试
    }
    throw std::runtime_error("Failed to create a unique session token after retries");
}
void RedisSessionManager::RefreshSessionTTL(const std::string& token) {
    auto r = redis_;
    if (!r) throw std::runtime_error("Redis not initialized");

    r->expire("session:" + token, std::chrono::seconds(defaultTTL_));
}

std::optional<int> RedisSessionManager::GetUserID(const std::string& token) {
    auto r = redis_;
    if (!r) return std::nullopt;

    auto val = r->get("session:" + token);
    if (val) {
        try { return std::stoi(*val); }
        catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

void RedisSessionManager::DeleteSession(const std::string& token) {
    auto r = redis_;
    if (!r) return;
    r->del("session:" + token);
}

// 高质量且无锁的随机：thread_local PRNG + 一次性播种
std::string RedisSessionManager::GenerateToken(int length) {
    static constexpr char kCharset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static constexpr int kCharsetMax = static_cast<int>(sizeof(kCharset) - 2);

    thread_local std::mt19937 gen([]{
        std::random_device rd;
        std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937(seed);
    }());
    std::uniform_int_distribution<int> dist(0, kCharsetMax);

    std::string token;
    token.resize(length);
    for (int i = 0; i < length; ++i) token[i] = kCharset[dist(gen)];
    return token;
}
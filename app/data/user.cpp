#include <sstream>

#if __GNUC__ >= 8
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#include "user.h"
#include "group.h"
#include "utils/logger.h"

#include "utils/rand.h"

#include "mirai/api.h"
#include "mirai/msg.h"

#include "yaml-cpp/yaml.h"

namespace user {

void peeCreateTable()
{
    if (db.exec(
        "CREATE TABLE IF NOT EXISTS pee( \
            qqid    INTEGER PRIMARY KEY NOT NULL, \
            currency INTEGER            NOT NULL, \
            cases   INTEGER             NOT NULL, \
            dailytime INTEGER           NOT NULL,  \
            keys    INTEGER             NOT NULL  \
         )") != SQLITE_OK)
        addLog(LOG_ERROR, "pee", db.errmsg());
}

void peeLoadFromDb()
{
    auto list = db.query("SELECT * FROM pee", 5);
    for (auto& row : list)
    {
        int64_t qq = std::any_cast<int64_t>(row[0]);
        int64_t p1, p2, p4;
        time_t  p3;
        p1 = std::any_cast<int64_t>(row[1]);
        p2 = std::any_cast<int64_t>(row[2]);
        p3 = std::any_cast<time_t> (row[3]);
        p4 = std::any_cast<int64_t>(row[4]);
        plist[qq] = { qq, p1, p2, p3, p4 };
    }
    addLogDebug("pee", "added %lu users", plist.size());
}

pdata::resultStamina pdata::getStamina(bool extra) const
{
    time_t t = time(nullptr);
    time_t last = stamina_recovery_time;
    int stamina = MAX_STAMINA;
    if (last > t) stamina -= (last - t) / STAMINA_TIME + !!((last - t) % STAMINA_TIME);
    if (extra) stamina += stamina_extra;
    return { true, stamina, stamina_recovery_time - t};
}

int pdata::getExtraStamina() const
{
    return stamina_extra;
}

pdata::resultStamina pdata::modifyStamina(int delta, bool extra)
{
    int stamina = getStamina(false).staminaAfterUpdate;
    int staminaOld = stamina;

    bool enough = false;

    if (delta < 0)
    {
        int cost = -delta;
        if (stamina_extra >= cost)
        {
            enough = true;
            stamina_extra -= cost;
        }
        else if (stamina + stamina_extra >= cost)
        {
            enough = true;
            cost -= stamina_extra;
            stamina_extra = 0;
            stamina -= cost;
        }
        else
        {
            enough = false;
        }
    }
    else if (delta > 0)
    {
        enough = true;
        if (extra) // directly goes to extra
        {
            stamina_extra += delta;
        }
        {
            stamina += delta;
            if (stamina >= MAX_STAMINA)
            {
                // if (extra) // part of cost(recovery) goes to extra
                // {
                //     stamina_extra += stamina - MAX_STAMINA;
                // }
                stamina = MAX_STAMINA;
            }
        }
    }

    time_t t = time(nullptr);
    time_t last = stamina_recovery_time;
    if (enough)
    {
        if (last > t)
            stamina_recovery_time += STAMINA_TIME * (staminaOld - stamina);
        else
            stamina_recovery_time = t + STAMINA_TIME * (staminaOld - stamina);

        if (stamina >= MAX_STAMINA) 
            stamina_recovery_time = t;
    }

    return { enough, stamina, stamina_recovery_time - t };
}

pdata::resultStamina pdata::testStamina(int cost) const
{
    int stamina = getStamina(false).staminaAfterUpdate;

    bool enough = false;

    if (cost > 0)
    {
        if (stamina_extra >= cost)
        {
            enough = true;
        }
        else if (stamina + stamina_extra >= cost)
        {
            enough = true;
        }
        else
        {
            enough = false;
        }
    }

    time_t t = time(nullptr);
    time_t last = stamina_recovery_time;
    if (enough)
    {
        if (last > t)
            last += STAMINA_TIME * cost;
        else
            last = t + STAMINA_TIME * cost;
    }

    if (enough && stamina >= MAX_STAMINA) last = t;
    return { enough, stamina, last - t };
}

void pdata::modifyCurrency(int64_t c)
{
    currency += c;
    if (currency < 0) currency = 0;
    db.exec("UPDATE pee SET currency=? WHERE qqid=?", { currency, qq });
}

void pdata::multiplyCurrency(double a)
{
    currency *= a;
    if (currency < 0) currency = 0;
    db.exec("UPDATE pee SET currency=? WHERE qqid=?", { currency, qq });
}

void pdata::modifyBoxCount(int64_t c)
{
    opened_box_count += c;
    db.exec("UPDATE pee SET cases=? WHERE qqid=?", { opened_box_count, qq });
}

void pdata::modifyDrawTime(time_t c)
{
    last_draw_time = c;
    db.exec("UPDATE pee SET dailytime=? WHERE qqid=?", { last_draw_time, qq });
}

void pdata::modifyKeyCount(int64_t c)
{
    key_count += c;
    if (key_count < 0) key_count = 0;
    db.exec("UPDATE pee SET keys=? WHERE qqid=?", { key_count, qq });
}

int pdata::createAccount(int64_t qqid, int64_t c)
{
    qq = qqid;
    currency = c;
    return db.exec("INSERT INTO pee(qqid, currency, cases, dailytime, keys) VALUES(? , ? , ? , ? , ?)",
        { qq, currency, 0, 0, 0 });
}

json not_registered(int64_t qq)
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessageAt(qq));
    resp["messageChain"].push_back(mirai::buildMessagePlain("，你还没有开通菠菜"));
    return resp;
}

json register_notify()
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessagePlain("是我要开通菠菜，你会不会开通菠菜"));
    return std::move(resp);
}

json already_registered(int64_t qq)
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessageAt(qq));
    resp["messageChain"].push_back(mirai::buildMessagePlain("，你已经开通过了"));
    return std::move(resp);
}

json registered(int64_t qq, int64_t balance)
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessageAt(qq));
    std::stringstream ss;
    ss << "，你可以开始开箱了，送给你" << balance << "个批";
    resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
    return std::move(resp);
}

json register_fail()
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessagePlain("开通失败，请联系管理员，，"));
    return std::move(resp);
}

json currency(int64_t qq, int64_t c, int64_t key, int stamina, time_t rtime, int stamina_extra = 0)
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessageAt(qq));
    std::stringstream ss;
    ss << "，你的余额为" << c << "个批，" << key << "把钥匙\n";
    resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
    ss.str("");
    if (stamina_extra > 0)
        ss << "你还有" << stamina << "(+" << stamina_extra << ")点体力";
    else
        ss << "你还有" << stamina << "点体力";
    if (stamina < user::MAX_STAMINA)
        ss << "，回满还需" << rtime / (60 * 60) << "小时" << rtime / 60 % 60 << "分钟";
    resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
    return std::move(resp);
}

json no_dailyp_flag()
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessagePlain("本群被隔离了，么得领批/cy"));
    return std::move(resp);
}

json have_already_drawn_today(int64_t qq)
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessageAt(qq));
    resp["messageChain"].push_back(mirai::buildMessagePlain("，你今天已经领过了，明天再来8"));
    return std::move(resp);
}

json draw_p(int64_t qq, int64_t base, int64_t bonus, int64_t daily_remain)
{
    json resp = R"({ "messageChain": [] })"_json;
    resp["messageChain"].push_back(mirai::buildMessageAt(qq));
    if (bonus)
    {
        std::stringstream ss;
        ss << "，你今天领到" << base << "个批，甚至还有先到的" << bonus << "个批\n";
        resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
        ss.str("");
        ss << "现在批池还剩" << daily_remain << "个批";
        resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
    }
    else
    {
        std::stringstream ss;
        ss << "，你今天领到" << base << "个批\n";
        resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
        ss.str("");
        ss << "每日批池么得了，明天请踩点";
        resp["messageChain"].push_back(mirai::buildMessagePlain(ss.str()));
    }
    return std::move(resp);
}

json REG(int64_t qq)
{
    if (plist.find(qq) != plist.end()) 
        return std::move(already_registered(qq));
    if (plist[qq].createAccount(qq, INITIAL_BALANCE))
    {
        return std::move(register_fail());
    }
    return std::move(registered(qq, INITIAL_BALANCE));
}

json REG_HINT(int64_t qq)
{
    if (plist.find(qq) != plist.end()) 
        return std::move(already_registered(qq));
    return std::move(register_notify());
}

json BALANCE(int64_t qq)
{
    if (plist.find(qq) == plist.end()) 
        return std::move(not_registered(qq));
    auto &p = plist[qq];
    auto[enough, stamina, rtime] = p.getStamina(false);
    return std::move(currency(qq, p.getCurrency(), p.getKeyCount(), stamina, rtime, p.getExtraStamina()));
}

json DRAW_P(int64_t qq, int64_t group)
{
    if (plist.find(qq) == plist.end()) 
        return std::move(not_registered(qq));
        
    if (!grp::groups[group].getFlag(grp::MASK_DAILYP))
        return std::move(no_dailyp_flag());
        
    auto &p = plist[qq];
    auto[enough, stamina, rtime] = p.getStamina(0);
    if (p.getLastDrawTime() > daily_refresh_time)
        return std::move(have_already_drawn_today(qq));
    
    int bonus = 0;
    if (remain_daily_bonus)
    {
        bonus = randInt(1, remain_daily_bonus > 66 ? 66 : remain_daily_bonus);
        remain_daily_bonus -= bonus;
    }
    p.modifyCurrency(FREE_BALANCE_ON_NEW_DAY + bonus);
    grp::groups[group].sum_earned += FREE_BALANCE_ON_NEW_DAY + bonus;
    p.modifyDrawTime(time(nullptr));
    return std::move(draw_p(qq, FREE_BALANCE_ON_NEW_DAY, bonus, remain_daily_bonus));
}

const std::map<std::string, commands> commands_str
{
    {"开通", commands::REG_HINT},
    {"开通菠菜", commands::REG_HINT},
    {"给我开通菠菜", commands::REG_HINT},
    {"注册", commands::REG_HINT},
    {"注册菠菜", commands::REG_HINT},
    {"我要注册菠菜", commands::REG_HINT},
    {"我要开通菠菜", commands::REG},
    {"余额", commands::BALANCE},
    {"领批", commands::DRAW_P},

    {"開通", commands::REG_HINT},           //繁體化
    {"開通菠菜", commands::REG_HINT},       //繁體化
    {"給我開通菠菜", commands::REG_HINT},   //繁體化
    {"註冊", commands::REG_HINT},           //繁體化
    {"註冊菠菜", commands::REG_HINT},       //繁體化
    {"我要註冊菠菜", commands::REG_HINT},   //繁體化
    {"我要開通菠菜", commands::REG},        //繁體化
    {"餘額", commands::BALANCE},            //繁體化
    {"領批", commands::DRAW_P},             //繁體化
};

void msgCallback(const json& body)
{
    auto query = mirai::messageChainToArgs(body);
    if (query.empty()) return;

    auto cmd = query[0];
    if (commands_str.find(cmd) == commands_str.end()) return;

    auto m = mirai::parseMsgMetadata(body);

    if (!grp::groups[m.groupid].getFlag(grp::MASK_P))
        return;

    json resp;
    switch (commands_str.at(cmd))
    {
    case commands::REG_HINT:
        resp = REG_HINT(m.qqid);
        break;
    case commands::REG:
        resp = REG(m.qqid);
        break;
    case commands::BALANCE:
        resp = BALANCE(m.qqid);
        break;
    case commands::DRAW_P:
        resp = DRAW_P(m.qqid, m.groupid);
        break;
    default: 
        break;
    }

    if (!resp.empty())
    {
        mirai::sendGroupMsg(m.groupid, resp);
    }
}

std::map<std::string, int64_t> USER_ALIAS;
int loadUserAlias(const char* yaml)
{
    fs::path cfgPath(yaml);
    if (!fs::is_regular_file(cfgPath))
    {
        addLog(LOG_ERROR, "user", "Alias config file %s not found", fs::absolute(cfgPath).c_str());
        return -1;
    }
    addLog(LOG_INFO, "user", "Loading alias config from %s", fs::absolute(cfgPath).c_str());

    YAML::Node cfg = YAML::LoadFile(yaml);
    unsigned c = 0;
    for (const auto& u: cfg)
    {
        int64_t qqid = u.first.as<int64_t>();
        for (const auto& a: u.second)
        {
            USER_ALIAS[a.as<std::string>()] = qqid;
            c++;
        }
    }
    addLog(LOG_INFO, "user", "Loaded %u entries", c);

    return 0;
}
int64_t getUser(const std::string& alias) 
{
    if (alias.empty()) return 0;

    if (USER_ALIAS.find(alias) != USER_ALIAS.end())
        return USER_ALIAS.at(alias);
    else if (alias[0] == '@')
        return std::strtoll(alias.substr(1).c_str(), nullptr, 10);
    else 
        return std::strtoll(alias.substr(0).c_str(), nullptr, 10);
}

void flushDailyTimep(bool autotriggered)
{
    daily_refresh_time = time(nullptr);
    //daily_refresh_tm = getLocalTime(TIMEZONE_HR, TIMEZONE_MIN);
    daily_refresh_tm = *localtime(&daily_refresh_time);
    if (autotriggered) daily_refresh_tm_auto = daily_refresh_tm;

    remain_daily_bonus = DAILY_BONUS + extra_tomorrow;
    extra_tomorrow = 0;

    std::vector<std::string> msgMap
    {
        "每日批池刷新了",
        "每日批池刷新了；额",
        "每日批次号刷新了",
        "没人批i吃刷新了",
        "每日P池刷新率；呃",
        "P 풀은 매일 새로 고쳐집니다",
        "may rii pee cii shruaa xinn laaar",
        "刷了"
    };

    grp::broadcastMsg(msgMap[randInt(int(msgMap.size()) - 1)].c_str(), grp::MASK_DAILYP);
    //CQ_addLog(ac, CQLOG_DEBUG, "pee", std::to_string(daily_refresh_time).c_str());
}

void init(const char* user_alias_yml)
{
    peeCreateTable();
    peeLoadFromDb();
    loadUserAlias(user_alias_yml);
}
}

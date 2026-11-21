#include "FluxPlayer/subtitle/TermEdits.h"
#include "FluxPlayer/subtitle/TranscriptBoundary.h"
#include <algorithm>
#include <map>
#include <utility>

namespace FluxPlayer {
namespace {
struct Syllable { const char* pinyin; const char* text; int tokens; };

// Each row lists characters read as the given syllable, with the number of
// BPE tokens the bundled base model spends on one such character. Only measured
// characters appear: an unknown character refuses a match instead of rewriting
// text that may already be correct. Readings collapse tones, so a wrong tone
// does not block a real term; particles that change meaning keep their tone.
constexpr Syllable kSyllables[] = {
    {"bo", "波播", 1}, {"bo", "博伯薄", 2}, {"bu", "不部步布", 1}, {"bu", "补", 2}, {"cai", "才", 1},
    {"cai", "材财采彩", 2}, {"can", "参残惨", 2}, {"chang", "长场常", 1}, {"chang", "厂昌畅", 2},
    {"che", "车撤彻", 2}, {"chen", "陈晨沉尘", 2}, {"cheng", "成程城", 1}, {"cheng", "称诚", 2},
    {"chi", "吃持", 1}, {"chi", "迟池尺", 2}, {"chu", "出除初楚", 1}, {"chu", "储础触", 2},
    {"chuan", "川", 1}, {"chuan", "传穿船串", 2}, {"chuang", "创窗床", 2}, {"ci", "次此", 1},
    {"ci", "词磁刺", 2}, {"cong", "从", 1}, {"cong", "丛", 2}, {"cun", "存村", 1}, {"cuo", "错", 1},
    {"cuo", "措", 2}, {"da", "大打答", 1}, {"da", "达", 2}, {"dai", "代待", 1}, {"dai", "带戴", 2},
    {"dan", "单但蛋", 1}, {"dan", "担淡", 2}, {"dao", "到道倒", 1}, {"dao", "导刀岛", 2},
    {"de", "的地得德", 1}, {"deng", "等登", 1}, {"deng", "灯邓", 2}, {"di", "第弟底低", 1},
    {"di", "敌抵", 2}, {"dian", "电点店", 1}, {"dian", "典殿碘", 2}, {"ding", "定", 1},
    {"ding", "订顶丁", 2}, {"dong", "动东懂", 1}, {"dong", "栋洞冬", 2}, {"du", "度毒", 1},
    {"du", "读渡肚独", 2}, {"duan", "段", 1}, {"duan", "断短端", 2}, {"dui", "对", 1}, {"dui", "队兑", 2},
    {"duo", "多", 1}, {"duo", "朵夺", 2}, {"en", "恩", 1}, {"er", "二而儿", 1}, {"er", "耳", 2},
    {"fa", "发法", 1}, {"fa", "罚伐", 2}, {"fan", "反翻", 1}, {"fan", "凡饭范返", 2},
    {"fang", "方房防放", 1}, {"fang", "访芳", 2}, {"fei", "非", 1}, {"fei", "飞费废肥", 2},
    {"fen", "分份粉", 1}, {"fen", "纷奋", 2}, {"feng", "风", 1}, {"feng", "封峰丰锋", 2},
    {"fu", "服府付夫父富", 1}, {"fu", "复副附负幅", 2}, {"gai", "改该", 1}, {"gai", "盖", 2},
    {"gan", "感干敢", 1}, {"gan", "赶", 2}, {"gao", "高告搞", 1}, {"gao", "稿", 2}, {"ge", "个各格哥歌", 1},
    {"ge", "隔割", 2}, {"geng", "更", 1}, {"geng", "耕", 2}, {"gong", "工公功共攻", 1},
    {"gong", "供宫", 2}, {"gou", "够构购沟勾", 2}, {"gu", "故古", 1}, {"gu", "股顾固孤鼓", 2},
    {"gua", "瓜", 1}, {"gua", "挂刮", 2}, {"guan", "关管观官", 1}, {"guan", "馆", 2},
    {"guang", "光", 1}, {"guang", "广逛", 2}, {"gui", "规归贵轨柜", 2}, {"guo", "过国果", 1},
    {"guo", "郭", 2}, {"hai", "还海害", 1}, {"han", "汉喊含韩函", 2}, {"hao", "好号", 1},
    {"hao", "毫豪", 2}, {"he", "和合何喝", 1}, {"he", "河核", 2}, {"hou", "后候", 1}, {"hou", "厚", 2},
    {"hu", "呼", 1}, {"hu", "户互湖护虎", 2}, {"hua", "话化花画", 1}, {"hua", "划华", 2}, {"huan", "欢", 1},
    {"huan", "换环缓", 2}, {"hui", "会回", 1}, {"hui", "汇惠恢", 2}, {"huo", "或火活", 1},
    {"huo", "获货", 2}, {"ji", "机及记即技己急基集", 1}, {"ji", "级几计极季积", 2}, {"jia", "家加假", 1},
    {"jia", "价甲架佳", 2}, {"jian", "间见件建健", 1}, {"jian", "简减检监", 2}, {"jiang", "将讲江", 1},
    {"jiang", "匠", 2}, {"jiao", "叫交教角", 1}, {"jiao", "较脚", 2}, {"jie", "接解界节结介街姐", 1},
    {"jin", "进今近金", 1}, {"jin", "尽仅紧", 2}, {"jing", "经京精境景警", 1}, {"jing", "静竞", 2},
    {"jiu", "就九酒救", 1}, {"jiu", "旧", 2}, {"ju", "局句具", 1}, {"ju", "据举聚巨", 2},
    {"juan", "卷捐", 2}, {"jue", "觉", 1}, {"jue", "决绝", 2}, {"kai", "开", 1}, {"kan", "看", 1},
    {"kan", "刊", 2}, {"ke", "可科客刻克", 1}, {"ke", "课", 2}, {"kong", "空控", 1}, {"kong", "孔", 2},
    {"ku", "苦", 1}, {"ku", "库", 2}, {"kuai", "快块", 1}, {"kuan", "款", 1}, {"kuan", "宽", 2},
    {"la", "拉啦", 1}, {"lai", "来", 1}, {"lai", "赖", 2}, {"lan", "蓝兰览烂", 2}, {"lao", "老", 1},
    {"lao", "劳牢", 2}, {"le", "了乐", 1}, {"lei", "累", 1}, {"lei", "类雷", 2}, {"li", "里理力利立李例", 1},
    {"li", "离历", 2}, {"lian", "联连练脸链", 2}, {"liang", "量两良亮", 1}, {"liao", "料聊", 1},
    {"lin", "林", 1}, {"lin", "临", 2}, {"ling", "令另", 1}, {"ling", "领零", 2}, {"liu", "六流留", 1},
    {"liu", "刘", 2}, {"long", "龙隆", 2}, {"lu", "路", 1}, {"lu", "录陆露卢", 2}, {"lun", "论轮", 2},
    {"luo", "落", 1}, {"luo", "罗络洛骆逻萝", 2}, {"ma", "吗马妈", 1}, {"ma", "码", 2}, {"mai", "买卖麦", 2},
    {"man", "慢", 1}, {"man", "满蛮", 2}, {"mao", "毛", 1}, {"mao", "冒贸", 2}, {"me", "么", 1},
    {"mei", "每没美妹", 1}, {"mei", "媒梅", 2}, {"men", "们", 1}, {"men", "门闷", 2}, {"mian", "面", 1},
    {"mian", "免", 2}, {"miao", "秒", 1}, {"miao", "描苗", 2}, {"ming", "名明命", 1}, {"mo", "模", 1},
    {"mo", "默磨末摸", 2}, {"mu", "目木母幕", 1}, {"mu", "募", 2}, {"na", "那拿哪", 1}, {"na", "纳", 2},
    {"nan", "南难男", 1}, {"nei", "内", 1}, {"neng", "能", 1}, {"ni", "你", 1}, {"ni", "尼泥逆", 2},
    {"nian", "年念", 1}, {"ning", "宁凝", 2}, {"nong", "农弄", 2}, {"nu", "努怒", 2}, {"nv", "女", 1},
    {"ou", "欧偶", 2}, {"pai", "排拍", 1}, {"pai", "派", 2}, {"pan", "盘判盼", 2}, {"pei", "配", 1},
    {"pei", "培赔", 2}, {"pian", "片", 1}, {"pian", "偏篇", 2}, {"ping", "平", 1},
    {"ping", "评苹凭", 2}, {"po", "破", 1}, {"po", "坡迫", 2}, {"pu", "普", 1}, {"pu", "铺朴谱", 2},
    {"qi", "期起其气器七奇", 1}, {"qi", "齐", 2}, {"qian", "前钱千", 1}, {"qian", "签浅", 2},
    {"qiang", "强抢墙", 2}, {"qing", "请清情青", 1}, {"qing", "轻", 2}, {"qiu", "求球秋", 1},
    {"qu", "去取曲", 1}, {"qu", "区趋", 2}, {"quan", "全", 1}, {"quan", "权劝圈", 2}, {"que", "确缺却", 2},
    {"rang", "让", 1}, {"re", "热", 2}, {"ren", "人任", 1}, {"ren", "认仁", 2}, {"ri", "日", 1},
    {"rong", "容", 1}, {"rong", "荣融", 2}, {"ru", "如入", 1}, {"ru", "乳", 2}, {"ruan", "软", 2},
    {"san", "三", 1}, {"san", "散伞", 2}, {"se", "色", 1}, {"se", "涩", 2}, {"sha", "杀沙莎纱砂傻", 2},
    {"shang", "上商", 1}, {"shang", "伤尚", 2}, {"shao", "少", 1}, {"shao", "烧绍勺韶", 2},
    {"she", "社", 1}, {"she", "设射舍蛇涉", 2}, {"shen", "身深什神甚", 1}, {"shen", "申伸绅沈审", 2},
    {"sheng", "生声升", 1}, {"sheng", "省胜圣绳", 2}, {"shi", "是时十事市实视石失师使始士示世式", 1},
    {"shi", "试识诗施湿狮拾驶氏势室适", 2}, {"shou", "手收受首守", 1}, {"shou", "熟寿授", 2}, {"shu", "数舒叔束", 1},
    {"shu", "书术树输属述署刷", 2}, {"shui", "水谁睡", 1}, {"shui", "税", 2}, {"shuo", "说", 1},
    {"si", "四死思司私", 1}, {"si", "丝寺", 2}, {"song", "送松", 1}, {"song", "宋", 2}, {"su", "素速诉", 1},
    {"su", "苏俗塑", 2}, {"suan", "算", 1}, {"suan", "酸", 2}, {"sui", "随岁虽碎", 2}, {"suo", "所", 1},
    {"suo", "索缩锁", 2}, {"ta", "他她它塔", 1}, {"tai", "太台", 1}, {"tai", "态抬", 2},
    {"tan", "谈探弹摊贪坦叹", 2}, {"tang", "糖", 1}, {"tang", "堂汤唐躺烫", 2}, {"ti", "体提题", 1},
    {"ti", "替梯踢", 2}, {"tian", "天田甜", 1}, {"tian", "填写添", 2}, {"tiao", "条跳挑", 1},
    {"tiao", "调", 2}, {"ting", "听停挺", 1}, {"ting", "厅庭", 2}, {"tong", "通同痛", 1},
    {"tong", "统铜", 2}, {"tou", "头投透", 1}, {"tu", "土突", 1}, {"tu", "图途吐兔", 2}, {"tuan", "团", 2},
    {"tuo", "拖脱托陀驼妥拓", 2}, {"wai", "外", 1}, {"wai", "歪", 2}, {"wan", "万完晚玩", 1},
    {"wan", "湾", 2}, {"wang", "往王忘望", 1}, {"wang", "网", 2}, {"wei", "为位未委微", 1},
    {"wei", "维卫", 2}, {"wen", "问文", 1}, {"wen", "温稳闻", 2}, {"wo", "我", 1}, {"wu", "无五物武午", 1},
    {"wu", "务误", 2}, {"xi", "系西喜洗", 1}, {"xi", "细习", 2}, {"xia", "下夏", 1}, {"xia", "霞", 2},
    {"xian", "先现", 1}, {"xian", "显线县险", 2}, {"xiang", "想相向香像", 1}, {"xiang", "项箱厢", 2},
    {"xiao", "小笑校效消", 1}, {"xie", "些谢", 1}, {"xie", "协", 2}, {"xin", "新心信", 1},
    {"xing", "行型性星形幸", 1}, {"xing", "兴", 2}, {"xiu", "休", 1}, {"xiu", "修", 2}, {"xu", "需", 1},
    {"xu", "许续序须", 2}, {"xuan", "选宣", 2}, {"xue", "学雪", 1}, {"ya", "呀", 1}, {"ya", "压亚牙押", 2},
    {"yan", "言眼演", 1}, {"yan", "验严烟变研", 2}, {"yang", "样", 1}, {"yang", "养洋阳", 2},
    {"yao", "要", 1}, {"yao", "药摇邀", 2}, {"ye", "也夜", 1}, {"ye", "业页", 2}, {"yi", "一已以意", 1},
    {"yi", "议移艺义", 2}, {"yin", "因音引印", 1}, {"yin", "银", 2}, {"ying", "应英影", 1},
    {"ying", "硬营", 2}, {"yong", "用永", 1}, {"yong", "拥勇", 2}, {"you", "有又由友右油", 1},
    {"you", "优邮", 2}, {"yu", "于", 1}, {"yu", "与语育域余预", 2}, {"yuan", "元原院源", 1},
    {"yuan", "员愿", 2}, {"yue", "月越", 1}, {"yue", "约阅", 2}, {"yun", "云运允", 2}, {"zai", "在再", 1},
    {"zai", "载灾", 2}, {"zan", "暂赞", 2}, {"zao", "早造", 1}, {"zao", "遭", 2}, {"ze", "则责择", 2},
    {"zen", "怎", 1}, {"zeng", "曾", 1}, {"zeng", "增", 2}, {"zhan", "站展", 1}, {"zhan", "占战", 2},
    {"zhang", "张", 1}, {"zhang", "章涨掌账", 2}, {"zhao", "找照", 1}, {"zhao", "招赵", 2},
    {"zhe", "这着者", 1}, {"zhe", "折", 2}, {"zhen", "真", 1}, {"zhen", "针阵镇振", 2},
    {"zheng", "正整政", 1}, {"zheng", "证征争", 2}, {"zhi", "只之知制指值直至治志支置", 1}, {"zhi", "职质址", 2},
    {"zhong", "中种重钟", 1}, {"zhong", "终众", 2}, {"zhou", "周州洲", 1}, {"zhou", "轴", 2},
    {"zhu", "主住注助著", 1}, {"zhu", "驻祝朱", 2}, {"zhuan", "专转砖", 2}, {"zhun", "准", 2},
    {"zhuo", "桌", 2}, {"zi", "字自子", 1}, {"zi", "资紫", 2}, {"zong", "总", 1}, {"zong", "综宗", 2},
    {"zou", "走", 1}, {"zou", "奏", 2}, {"zu", "足", 1}, {"zu", "组族祖", 2}, {"zui", "最", 1},
    {"zui", "嘴罪", 2}, {"zuo", "做作坐左", 1}, {"zuo", "座", 2}, {"lv", "绿旅律", 2}, {"lv", "率", 1},
    {"shan", "山", 1}, {"shan", "善闪删", 2}, {"te", "特", 1}, {"tui", "推退", 1}, {"tui", "腿", 2},
    {"shuo", "朔硕烁", 2}, {"sai", "赛塞腮", 2}, {"sen", "森僧", 2}, {"shuai", "衰甩帅", 2},
    {"shuang", "双霜爽", 2}, {"shun", "瞬顺", 2}, {"sou", "搜", 2}, {"sun", "孙损", 2},
    {"ta", "塌踏", 2}, {"tao", "掏逃桃讨套", 2}, {"teng", "疼腾", 2}, {"tie", "贴", 2}, {"tou", "偷", 2},
    {"tun", "吞", 2}, {"mi", "秘", 2}, {"kou", "扣", 2}, {"mi", "密", 1}, {"kou", "口", 1},
};

uint32_t codepoint(const char*& cursor) {
    const auto lead = static_cast<unsigned char>(*cursor++);
    if (lead < 0x80) return lead;
    const int length = (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 : 3;
    uint32_t value = lead & ((1u << (6 - length)) - 1);
    for (int i = 0; i < length; ++i) value = (value << 6) | (static_cast<unsigned char>(*cursor++) & 0x3f);
    return value;
}

// Tone-bearing particles change meaning with intonation; everything else uses
// the syllable alone so a wrong tone does not block a real match.
uint32_t toneOf(uint32_t value) {
    switch (value) {
        case 0x5417: /*吗*/ return 1;
        case 0x4EEC: /*们*/ return 1;
        case 0x773C: /*眼*/ return 1;
        case 0x4E86: /*了*/ return 1;
        default: return 0;
    }
}

// Stable slot per distinct reading, numbered in table order. Hashing collides
// (lu/luo differ by one bit) and a collision between two real syllables would
// make a rule match the wrong sound, so readings get explicit slots instead.
uint32_t slotOf(const char* pinyin) {
    static const auto slots = [] {
        std::map<std::string, uint32_t> built;
        for (const auto& entry : kSyllables) built.emplace(entry.pinyin, 0);
        uint32_t next = 1;
        for (auto& item : built) item.second = next++;
        return built;
    }();
    const auto found = slots.find(pinyin);
    return found == slots.end() ? 0 : found->second;
}
}

bool TermEdits::Sound::operator==(const Sound& other) const {
    // Token cost only ranks candidates; it must not block a real homophone.
    return length == other.length &&
        std::equal(slots.begin(), slots.begin() + length, other.slots.begin());
}

TermEdits::Sound TermEdits::pinyinKey(const std::string& text) {
    static const auto readings = [] {
        std::map<uint32_t, std::pair<uint32_t, int>> built;
        for (const auto& entry : kSyllables) {
            const auto slot = slotOf(entry.pinyin);
            if (!slot || slot >= 400) continue; // 1000+ is reserved for tones.
            for (const char* c = entry.text; *c;) {
                const auto value = codepoint(c);
                const auto found = built.find(value);
                if (found == built.end() || entry.tokens < found->second.second)
                    built[value] = {slot, entry.tokens};
            }
        }
        return built;
    }();
    Sound sound;
    for (const auto& character : TranscriptBoundary::characters(text)) {
        const auto found = readings.find(character.value);
        // Latin and kana terms, and characters absent from the measurements,
        // compare by text instead of sound.
        if (found == readings.end() || sound.length >= sound.slots.size()) return {};
        const auto slot = toneOf(character.value) ? 1000u + toneOf(character.value) : found->second.first;
        sound.slots[sound.length++] = slot;
        sound.cost += found->second.second;
    }
    // A single character cannot disambiguate homophones well enough.
    if (sound.length < 2) return {};
    return sound;
}

bool TermEdits::parse(const std::string& spec) {
    rules_.clear();
    size_t cursor = 0;
    while (cursor <= spec.size() && rules_.size() < 16) {
        const auto next = spec.find_first_of(",，;\n", cursor);
        auto rule = spec.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        cursor = next == std::string::npos ? spec.size() + 1 : next + 1;
        const auto trim = [](std::string& value) {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        };
        trim(rule);
        const auto split = rule.find('=');
        if (split == std::string::npos) continue;
        Rule entry{rule.substr(0, split), rule.substr(split + 1), {}};
        trim(entry.wrong);
        trim(entry.right);
        const auto wrong = TranscriptBoundary::characters(entry.wrong).size();
        const auto right = TranscriptBoundary::characters(entry.right).size();
        // One character collides with ordinary speech; very long forms are typos.
        if (wrong < 2 || wrong > 12 || right < 2 || right > 12 || entry.wrong == entry.right) continue;
        entry.pinyin = pinyinKey(entry.wrong);
        if (!entry.pinyin.length) continue;
        const bool duplicate = std::any_of(rules_.begin(), rules_.end(), [&](const Rule& other) {
            return other.wrong == entry.wrong && other.right == entry.right;
        });
        if (!duplicate) rules_.push_back(std::move(entry));
    }
    return !rules_.empty();
}

std::string TermEdits::apply(const std::string& text, const std::string& heard) const {
    if (rules_.empty() || text.empty()) return text;
    const auto chars = TranscriptBoundary::characters(text);
    const auto support = TranscriptBoundary::characters(heard);
    struct Hit { size_t begin; size_t end; const Rule* rule; };
    std::vector<Hit> hits;
    for (const auto& rule : rules_) {
        for (size_t at = text.find(rule.wrong); at != std::string::npos;
             at = text.find(rule.wrong, at + 1)) {
            // Locate the rule in character space, then look for a same-length
            // window starting within two characters in what the audio heard.
            size_t position = chars.size();
            for (size_t i = 0; i < chars.size(); ++i)
                if (chars[i].byte == at) { position = i; break; }
            if (position == chars.size()) continue;
            bool supported = false;
            const auto length = rule.pinyin.length;
            for (size_t start = position > 2 ? position - 2 : 0;
                 start <= position + 2 && start + length <= support.size() && !supported; ++start) {
                // Slice the heard text in bytes so the evidence window cannot
                // drift when the two strings differ in length.
                const auto from = support[start].byte;
                const auto to = start + length < support.size() ?
                    support[start + length].byte : heard.size();
                supported = to > from && pinyinKey(heard.substr(from, to - from)) == rule.pinyin;
            }
            if (supported) hits.push_back({at, at + rule.wrong.size(), &rule});
        }
    }
    if (hits.empty()) return text;
    // Apply backwards; overlapping hits keep the rightmost one.
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.begin > b.begin; });
    std::string result = text;
    size_t limit = std::string::npos;
    std::vector<std::pair<size_t, size_t>> replaced; // output range of each fix
    for (const auto& hit : hits) {
        if (hit.end > limit) continue;
        // A replacement produced by another rule must never be corrected
        // again ("A=B" then "B=C"): skip this hit if it lands inside one.
        const bool chained = std::any_of(replaced.begin(), replaced.end(), [&](const auto& range) {
            return hit.begin >= range.first && hit.begin < range.second;
        });
        if (chained) continue;
        result.replace(hit.begin, hit.end - hit.begin, hit.rule->right);
        replaced.push_back({hit.begin, hit.begin + hit.rule->right.size()});
        limit = hit.begin;
    }
    return result;
}
}

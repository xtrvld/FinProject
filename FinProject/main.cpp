#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;
using std::string;
using std::vector;

static const fs::path VCS_DIR = ".simple_vcs";
static const fs::path OBJECTS_DIR = VCS_DIR / "objects";
static const fs::path REFS_HEADS_DIR = VCS_DIR / "refs" / "heads";
static const fs::path HEAD_FILE = VCS_DIR / "HEAD";

// ---------------- SHA1 ----------------
static string sha1(const vector<uint8_t>& data) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    uint64_t original_bits = static_cast<uint64_t>(data.size()) * 8ULL;

    vector<uint8_t> buf = data;
    buf.push_back(0x80);
    while ((buf.size() * 8ULL) % 512ULL != 448ULL) buf.push_back(0x00);

    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((original_bits >> (i * 8)) & 0xFF));
    }

    auto rol1 = [](uint32_t x) { return (x << 1) | (x >> 31); };
    auto rol5 = [](uint32_t x) { return (x << 5) | (x >> 27); };
    auto rol30 = [](uint32_t x) { return (x << 30) | (x >> 2); };

    for (size_t chunk = 0; chunk < buf.size(); chunk += 64) {
        uint32_t w[80]{};

        for (int i = 0; i < 16; ++i) {
            size_t idx = chunk + i * 4;
            w[i] = (uint32_t(buf[idx]) << 24) |
                (uint32_t(buf[idx + 1]) << 16) |
                (uint32_t(buf[idx + 2]) << 8) |
                (uint32_t(buf[idx + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = rol1(x);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }

            uint32_t temp = rol5(a) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol30(b);
            b = a;
            a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    char out[41];
    std::snprintf(out, sizeof(out), "%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4);
    return string(out);
}

// ---------------- utils ----------------
static bool isRepo() {
    return fs::exists(VCS_DIR) && fs::exists(OBJECTS_DIR) && fs::exists(REFS_HEADS_DIR) && fs::exists(HEAD_FILE);
}

static bool readAllBytes(const fs::path& p, vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

static bool writeAllBytes(const fs::path& p, const vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return true;
}

static string readFirstLine(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return "";
    string line;
    std::getline(in, line);
    return line;
}

static bool writeText(const fs::path& p, const string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    if (!out) return false;
    out << s;
    return true;
}

static string nowTimestamp() {
    auto tp = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return string(buf);
}

// ---------------- HEAD / branch ----------------
static bool headIsRef(string& outRefPath) {
    string head = readFirstLine(HEAD_FILE);
    const string prefix = "ref: ";
    if (head.rfind(prefix, 0) == 0) {
        outRefPath = head.substr(prefix.size()); // refs/heads/master
        return true;
    }
    return false;
}

static string currentCommitFromHEAD() {
    string ref;
    if (headIsRef(ref)) {
        return readFirstLine(VCS_DIR / ref);
    }
    // detached HEAD
    return readFirstLine(HEAD_FILE);
}

static string currentBranchNameOrEmpty() {
    string ref;
    if (!headIsRef(ref)) return "";
    auto pos = ref.find_last_of('/');
    if (pos == string::npos) return "";
    return ref.substr(pos + 1);
}

// ---------------- objects ----------------
static string writeObjectIfMissing(const vector<uint8_t>& content) {
    string h = sha1(content);
    fs::path objPath = OBJECTS_DIR / h;
    if (!fs::exists(objPath)) writeAllBytes(objPath, content);
    return h;
}

static string makeBlobForFile(const fs::path& filePath) {
    vector<uint8_t> bytes;
    if (!readAllBytes(filePath, bytes)) return "";
    return writeObjectIfMissing(bytes);
}

// Tree = список всех файлов проекта (без .simple_vcs):
// blob <hash> <relative_path>
static string makeTreeObject(std::unordered_map<string, string>& outTreeMap) {
    outTreeMap.clear();

    fs::path root = fs::current_path();
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        const fs::path p = it->path();

        // пропускаем .simple_vcs целиком
        if (p.filename() == VCS_DIR.filename()) {
            it.disable_recursion_pending();
            continue;
        }

        if (!it->is_regular_file()) continue;

        fs::path rel = fs::relative(p, root);
        // всегда храним с forward slash, чтобы одинаково читалось
        string relStr = rel.generic_string();

        string blobHash = makeBlobForFile(p);
        if (!blobHash.empty()) outTreeMap[relStr] = blobHash;
    }

    // сформировать текст tree-объекта
    vector<uint8_t> treeBytes;
    {
        std::vector<string> keys;
        keys.reserve(outTreeMap.size());
        for (auto& kv : outTreeMap) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        std::ostringstream oss;
        for (auto& k : keys) {
            oss << "blob " << outTreeMap[k] << " " << k << "\n";
        }
        string txt = oss.str();
        treeBytes.assign(txt.begin(), txt.end());
    }

    return writeObjectIfMissing(treeBytes);
}

static bool readTreeObject(const string& treeHash, std::unordered_map<string, string>& outTreeMap) {
    outTreeMap.clear();
    vector<uint8_t> bytes;
    if (!readAllBytes(OBJECTS_DIR / treeHash, bytes)) return false;
    string txt(bytes.begin(), bytes.end());
    std::istringstream iss(txt);
    string type, hash, path;
    while (iss >> type >> hash) {
        std::getline(iss, path);
        if (!path.empty() && path[0] == ' ') path.erase(path.begin());
        if (type == "blob" && !path.empty()) outTreeMap[path] = hash;
    }
    return true;
}

static bool readCommitObject(const string& commitHash, string& outTreeHash, string& outParentHash, string& outMessage) {
    outTreeHash.clear(); outParentHash.clear(); outMessage.clear();

    vector<uint8_t> bytes;
    if (!readAllBytes(OBJECTS_DIR / commitHash, bytes)) return false;
    string txt(bytes.begin(), bytes.end());
    std::istringstream iss(txt);
    string key;
    while (iss >> key) {
        if (key == "tree") iss >> outTreeHash;
        else if (key == "parent") iss >> outParentHash;
        else if (key == "message") {
            std::getline(iss, outMessage);
            if (!outMessage.empty() && outMessage[0] == ' ') outMessage.erase(outMessage.begin());
        }
        else {
            string dummy;
            std::getline(iss, dummy);
        }
    }
    return !outTreeHash.empty();
}

static string makeCommitObject(const string& treeHash, const string& parentHash, const string& message) {
    // формат упрощенный, но по смыслу соответствует заданию
    std::ostringstream oss;
    oss << "tree " << treeHash << "\n";
    if (!parentHash.empty()) oss << "parent " << parentHash << "\n";
    oss << "author Student\n";
    oss << "timestamp " << nowTimestamp() << "\n";
    oss << "message " << message << "\n";
    string txt = oss.str();
    vector<uint8_t> bytes(txt.begin(), txt.end());
    return writeObjectIfMissing(bytes);
}

// ---------------- checkout restore ----------------
static void cleanWorkingDir() {
    for (auto& entry : fs::directory_iterator(fs::current_path())) {
        if (entry.path().filename() == VCS_DIR.filename()) continue;
        fs::remove_all(entry.path());
    }
}

static bool restoreFromTree(const std::unordered_map<string, string>& treeMap) {
    for (auto& kv : treeMap) {
        fs::path relPath = fs::path(kv.first);
        fs::path fullPath = fs::current_path() / relPath;

        fs::create_directories(fullPath.parent_path());

        vector<uint8_t> blobBytes;
        if (!readAllBytes(OBJECTS_DIR / kv.second, blobBytes)) return false;
        if (!writeAllBytes(fullPath, blobBytes)) return false;
    }
    return true;
}

// ---------------- diff (LCS) ----------------
struct DiffOp { char t; string line; };

static vector<string> splitLines(const vector<uint8_t>& bytes) {
    string s(bytes.begin(), bytes.end());
    vector<string> lines;
    std::istringstream iss(s);
    string line;
    while (std::getline(iss, line)) {
        // без '\n'
        lines.push_back(line);
    }
    // если файл заканчивается без \n - getline всё равно даст последнюю строку
    return lines;
}

static vector<DiffOp> lcsDiff(const vector<string>& A, const vector<string>& B) {
    int m = (int)A.size(), n = (int)B.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));

    for (int i = m - 1; i >= 0; --i) {
        for (int j = n - 1; j >= 0; --j) {
            if (A[i] == B[j]) dp[i][j] = dp[i + 1][j + 1] + 1;
            else dp[i][j] = std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }

    vector<DiffOp> ops;
    int i = 0, j = 0;
    while (i < m || j < n) {
        if (i < m && j < n && A[i] == B[j]) { ops.push_back({ ' ', A[i] }); ++i; ++j; }
        else if (j < n && (i == m || dp[i][j + 1] >= dp[i + 1][j])) { ops.push_back({ '+', B[j] }); ++j; }
        else { ops.push_back({ '-', A[i] }); ++i; }
    }
    return ops;
}

static bool loadFileLinesFromBlob(const string& blobHash, vector<string>& outLines) {
    vector<uint8_t> bytes;
    if (!readAllBytes(OBJECTS_DIR / blobHash, bytes)) return false;
    outLines = splitLines(bytes);
    return true;
}

static bool loadFileLinesFromWork(const fs::path& rel, vector<string>& outLines) {
    vector<uint8_t> bytes;
    if (!readAllBytes(fs::current_path() / rel, bytes)) return false;
    outLines = splitLines(bytes);
    return true;
}

static void printUnifiedDiffSimple(const string& file, const vector<string>& A, const vector<string>& B) {
    // если одинаковые, не печатаем
    if (A == B) return;

    std::cout << "diff --simple-vcs a/" << file << " b/" << file << "\n";
    std::cout << "--- a/" << file << "\n";
    std::cout << "+++ b/" << file << "\n";

    // один большой ханк (упрощение для зачета)
    std::cout << "@@ -1," << A.size() << " +1," << B.size() << " @@\n";

    auto ops = lcsDiff(A, B);
    for (auto& op : ops) {
        if (op.t == ' ') continue; // контекст можно не печатать (упрощенно)
        std::cout << op.t << op.line << "\n";
    }
}

// ---------------- commands ----------------
static int cmd_init() {
    fs::create_directories(OBJECTS_DIR);
    fs::create_directories(REFS_HEADS_DIR);
    writeText(HEAD_FILE, "ref: refs/heads/master");
    writeText(REFS_HEADS_DIR / "master", "");
    std::cout << "Repo initialized: .simple_vcs\n";
    return 0;
}

static int cmd_commit(int argc, char* argv[]) {
    if (!isRepo()) { std::cerr << "Not a repo. Run init.\n"; return 1; }

    string msg;
    for (int i = 2; i < argc; ++i) {
        if (string(argv[i]) == "-m" && i + 1 < argc) {
            for (int j = i + 1; j < argc; ++j) {
                msg += argv[j];
                if (j + 1 < argc) msg += " ";
            }
            break;
        }
    }
    if (msg.empty()) { std::cerr << "Usage: commit -m \"message\"\n"; return 1; }

    string ref;
    if (!headIsRef(ref)) {
        std::cerr << "Detached HEAD: commit is not allowed (simplified).\n";
        return 1;
    }

    string parent = readFirstLine(VCS_DIR / ref);

    std::unordered_map<string, string> treeMap;
    string treeHash = makeTreeObject(treeMap);
    string commitHash = makeCommitObject(treeHash, parent, msg);

    writeText(VCS_DIR / ref, commitHash);

    std::cout << "Committed: " << commitHash.substr(0, 8) << " - \"" << msg << "\"\n";
    return 0;
}

static int cmd_checkout(int argc, char* argv[]) {
    if (!isRepo()) { std::cerr << "Not a repo. Run init.\n"; return 1; }
    if (argc < 3) { std::cerr << "Usage: checkout <commit_hash|branch>\n"; return 1; }

    string target = argv[2];
    string commitHash;
    bool isBranch = false;

    fs::path branchPath = REFS_HEADS_DIR / target;
    if (fs::exists(branchPath)) {
        isBranch = true;
        commitHash = readFirstLine(branchPath);
    }
    else {
        commitHash = target;
    }

    if (commitHash.empty()) {
        std::cerr << "Target has no commits.\n";
        return 1;
    }
    if (!fs::exists(OBJECTS_DIR / commitHash)) {
        std::cerr << "Commit not found: " << commitHash << "\n";
        return 1;
    }

    string treeHash, parentHash, message;
    if (!readCommitObject(commitHash, treeHash, parentHash, message)) {
        std::cerr << "Bad commit object.\n";
        return 1;
    }

    std::unordered_map<string, string> treeMap;
    if (!readTreeObject(treeHash, treeMap)) {
        std::cerr << "Bad tree object.\n";
        return 1;
    }

    cleanWorkingDir();
    if (!restoreFromTree(treeMap)) {
        std::cerr << "Restore failed.\n";
        return 1;
    }

    if (isBranch) writeText(HEAD_FILE, "ref: refs/heads/" + target);
    else writeText(HEAD_FILE, commitHash);

    std::cout << "Checked out " << (isBranch ? "branch " : "commit ") << (isBranch ? target : commitHash.substr(0, 8)) << "\n";
    return 0;
}

static int cmd_branch(int argc, char* argv[]) {
    if (!isRepo()) { std::cerr << "Not a repo. Run init.\n"; return 1; }

    if (argc == 2) {
        string cur = currentBranchNameOrEmpty();
        for (auto& entry : fs::directory_iterator(REFS_HEADS_DIR)) {
            string name = entry.path().filename().string();
            if (name == cur) std::cout << "* " << name << "\n";
            else std::cout << "  " << name << "\n";
        }
        return 0;
    }

    string arg2 = argv[2];
    if (arg2 == "-d") {
        if (argc < 4) { std::cerr << "Usage: branch -d <name>\n"; return 1; }
        string del = argv[3];
        if (del == currentBranchNameOrEmpty()) {
            std::cerr << "Can't delete current branch.\n";
            return 1;
        }
        fs::path p = REFS_HEADS_DIR / del;
        if (!fs::exists(p)) { std::cerr << "No such branch.\n"; return 1; }
        fs::remove(p);
        std::cout << "Deleted branch " << del << "\n";
        return 0;
    }

    // create branch
    fs::path newB = REFS_HEADS_DIR / arg2;
    if (fs::exists(newB)) { std::cerr << "Branch exists.\n"; return 1; }

    string curCommit = currentCommitFromHEAD();
    writeText(newB, curCommit);
    std::cout << "Created branch " << arg2 << "\n";
    return 0;
}

static int cmd_log(int argc, char* argv[]) {
    if (!isRepo()) { std::cerr << "Not a repo. Run init.\n"; return 1; }

    bool oneline = (argc >= 3 && string(argv[2]) == "--oneline");
    string c = currentCommitFromHEAD();
    if (c.empty()) { std::cout << "No commits.\n"; return 0; }

    while (!c.empty()) {
        string treeHash, parentHash, msg;
        if (!readCommitObject(c, treeHash, parentHash, msg)) break;

        if (oneline) {
            std::cout << c.substr(0, 8) << " " << msg << "\n";
        }
        else {
            std::cout << "commit " << c << "\n";
            std::cout << "    " << msg << "\n\n";
        }
        c = parentHash;
    }
    return 0;
}

static int cmd_diff(int argc, char* argv[]) {
    if (!isRepo()) { std::cerr << "Not a repo. Run init.\n"; return 1; }

    // diff modes:
    // diff                 => HEAD vs working
    // diff <commit|branch> => commit vs working
    // diff <A> <B>         => A vs B
    auto resolveToCommit = [](const string& s) -> string {
        fs::path bp = REFS_HEADS_DIR / s;
        if (fs::exists(bp)) return readFirstLine(bp);
        return s;
        };

    string Acommit, Bcommit;
    bool Bworking = false;

    if (argc == 2) {
        Acommit = currentCommitFromHEAD();
        Bworking = true;
    }
    else if (argc == 3) {
        Acommit = resolveToCommit(argv[2]);
        Bworking = true;
    }
    else {
        Acommit = resolveToCommit(argv[2]);
        Bcommit = resolveToCommit(argv[3]);
    }

    auto loadTreeFromCommit = [](const string& commitHash, std::unordered_map<string, string>& outTree) -> bool {
        string treeHash, parent, msg;
        if (!readCommitObject(commitHash, treeHash, parent, msg)) return false;
        return readTreeObject(treeHash, outTree);
        };

    std::unordered_map<string, string> treeA, treeB;
    if (Acommit.empty() || !fs::exists(OBJECTS_DIR / Acommit) || !loadTreeFromCommit(Acommit, treeA)) {
        std::cerr << "Bad commit A.\n";
        return 1;
    }

    if (!Bworking) {
        if (Bcommit.empty() || !fs::exists(OBJECTS_DIR / Bcommit) || !loadTreeFromCommit(Bcommit, treeB)) {
            std::cerr << "Bad commit B.\n";
            return 1;
        }
    }
    else {
        // working tree: mark files with empty hash, read from disk
        for (auto it = fs::recursive_directory_iterator(fs::current_path()); it != fs::recursive_directory_iterator(); ++it) {
            if (it->path().filename() == VCS_DIR.filename()) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file()) continue;
            string rel = fs::relative(it->path(), fs::current_path()).generic_string();
            treeB[rel] = ""; // empty => working file
        }
    }

    std::set<string> all;
    for (auto& kv : treeA) all.insert(kv.first);
    for (auto& kv : treeB) all.insert(kv.first);

    for (auto& file : all) {
        vector<string> linesA, linesB;

        // A always from blobs
        auto itA = treeA.find(file);
        if (itA != treeA.end()) loadFileLinesFromBlob(itA->second, linesA);

        // B from blobs or working
        auto itB = treeB.find(file);
        if (itB != treeB.end()) {
            if (!itB->second.empty()) loadFileLinesFromBlob(itB->second, linesB);
            else loadFileLinesFromWork(fs::path(file), linesB);
        }

        printUnifiedDiffSimple(file, linesA, linesB);
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: simple_vcs <init|commit|checkout|diff|branch|log>\n";
        return 1;
    }

    string cmd = argv[1];
    if (cmd == "init") return cmd_init();
    if (cmd == "commit") return cmd_commit(argc, argv);
    if (cmd == "checkout") return cmd_checkout(argc, argv);
    if (cmd == "diff") return cmd_diff(argc, argv);
    if (cmd == "branch") return cmd_branch(argc, argv);
    if (cmd == "log") return cmd_log(argc, argv);

    std::cerr << "Unknown command: " << cmd << "\n";
    return 1;
}

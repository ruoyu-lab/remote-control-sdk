// Relative paths items carry: what is taken and what is not, on every system;
// the names RENAME tries; the machine's own paths.
#include "files.hpp"
#include "paths.hpp"
#include "support/check.hpp"

using namespace rcsdk::file_transfer;

namespace {

bool taken(std::string_view path) {
    return !pathProblem(path).has_value();
}

void relativePaths() {
    for (const char* good : {"a", "a.txt", "dir/sub/file.tar.gz", ".profile", "a b", "x..y", "名字/文件.txt",
                             "COM10", "LPT", "console", "nul2.txt", "CON_", "a/b/c/d/e/f"}) {
        if (!taken(good)) {
            std::fprintf(stderr, "refused %s: %s\n", good, pathProblem(good)->c_str());
            CHECK(false);
        }
    }
    for (const char* bad : {"", "/", "/etc/passwd", "a/", "/a", "a//b", ".", "..", "../a", "a/../b", "a/./b",
                            "C:", "C:/Windows", "c:a", "a\\b", "\\\\server\\share", "a:b", "a*", "a?", "a\"b",
                            "a<b", "a>b", "a|b", "a.", "a ", "dir./x", "CON", "con", "Con.txt", "aux.tar.gz",
                            "NUL", "prn", "COM1", "com9.log", "LPT1", "lpt9", "COM0", "LPT0", "CONIN$",
                            "conout$.txt", "CON .txt", "a/nul/b", "a\x01", "tab\tname", "a\x7f",
                            "x.rcsdk-part", "dir.rcsdk-part/a", ".rcsdk-resume-00", "a/.rcsdk-resume-x"}) {
        if (taken(bad)) {
            std::fprintf(stderr, "took \"%s\"\n", bad);
            CHECK(false);
        }
    }
    CHECK(!taken("COM\xC2\xB9") && !taken("lpt\xC2\xB2.txt") && !taken("COM\xC2\xB3"));
    CHECK(!taken("\xff\xfe"));                 // Not UTF-8.
    CHECK(!taken("\xc0\xaf"));                 // An overlong '/'.
    CHECK(!taken("\xed\xa0\x80"));             // A surrogate.
    CHECK(taken(std::string(255, 'a')) && !taken(std::string(256, 'a')));
    std::string longest;
    while (longest.size() + 101 < RCS_FILE_PATH_MAX) {
        longest += std::string(100, 'p') + "/";
    }
    longest += std::string(RCS_FILE_PATH_MAX - 1 - longest.size(), 'q');
    CHECK(longest.size() == RCS_FILE_PATH_MAX - 1 && taken(longest) && !taken(longest + "q"));

    CHECK(components("a/b/c") == std::vector<std::string>({"a", "b", "c"}));
    CHECK(lastComponent("a/b/c.txt") == "c.txt" && lastComponent("c") == "c");
    CHECK(parentOf("a/b/c.txt") == "a/b" && parentOf("c").empty());
}

void renamedNames() {
    CHECK(numbered("report.pdf", 1) == "report (1).pdf");
    CHECK(numbered("a/b/archive.tar.gz", 2) == "a/b/archive.tar (2).gz");
    CHECK(numbered(".profile", 3) == ".profile (3)");
    CHECK(numbered("README", 12) == "README (12)");
    // Too long: the stem gives way, a whole character at a time.
    const std::string name = std::string(250, 'n') + ".txt";
    const std::string renamed = numbered(name, 7);
    CHECK(renamed.size() <= maxComponent && taken(renamed) && renamed.substr(renamed.size() - 8) == " (7).txt");
    std::string wide;
    while (wide.size() < 250) {
        wide += "\xE5\x90\x8D"; // A three-byte character.
    }
    const std::string cut = numbered(wide + ".bin", 1);
    CHECK(cut.size() <= maxComponent && taken(cut));
}

void machinePaths() {
#ifdef _WIN32
    CHECK(files::absolute("C:\\Users") && files::absolute("c:/x") && files::absolute("\\\\server\\share\\x"));
    CHECK(!files::absolute("C:x") && !files::absolute("x\\y") && !files::absolute("\\x"));
    CHECK(files::join("C:\\Downloads\\", "a/b.txt") == "C:\\Downloads\\a\\b.txt");
    CHECK(files::lastName("C:\\Users\\me\\file.txt") == "file.txt" && files::lastName("C:\\dir\\") == "dir");
    CHECK(files::lastName("C:\\").empty());
#else
    CHECK(files::absolute("/tmp") && !files::absolute("tmp") && !files::absolute(""));
    CHECK(files::join("/tmp/", "a/b.txt") == "/tmp/a/b.txt" && files::join("/", "a") == "/a");
    CHECK(files::lastName("/home/me/file.txt") == "file.txt" && files::lastName("/home/me/dir/") == "dir");
    CHECK(files::lastName("/").empty());
#endif
}

} // namespace

int main() {
    return runTest("file_transfer.paths", [] {
        relativePaths();
        renamedNames();
        machinePaths();
    });
}

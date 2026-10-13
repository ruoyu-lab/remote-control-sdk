// The browser package's JSON (web/json.hpp): what rcsdk.mjs writes is read
// back, malformed or too deeply nested text reads as null, numbers convert
// within their types' ranges, and the events' JSON is valid.
#include "../web/json.hpp"
#include "support/check.hpp"

#include <cmath>
#include <cstdint>
#include <string>

using web::Json;

int main() {
    return runTest("web_json", [] {
        const Json options = Json::parse(
            R"({"identity":{"key":"00ff"},"decoder":{"codecs":[1,2],"acceleration":0},"files":{"send":true,)"
            R"("maxOfferBytes":5000000000},"name":"a \"b\" 中 😀\n","none":null,"neg":-3})");
        CHECK(options.kind() == Json::Object);
        CHECK(options["identity"]["key"].text() == "00ff");
        CHECK(options["decoder"]["codecs"].items().size() == 2 && options["decoder"]["codecs"].items()[1].u32() == 2);
        CHECK(options["decoder"]["acceleration"].u32(7) == 0 && options["decoder"]["absent"].u32(7) == 7);
        CHECK(options["files"]["send"].boolean() && options["files"]["maxOfferBytes"].u64() == 5000000000ull);
        CHECK(options["name"].text() == "a \"b\" \xe4\xb8\xad \xf0\x9f\x98\x80\n"); // UTF-8, a surrogate pair.
        CHECK(!options.has("none") && !options.has("absent") && options["neg"].u32() == 0);
        CHECK(options["name"]["field"].kind() == Json::Null); // A field of a string: null.
        // Malformed: null, never a part of it.
        for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\":1} x", "\"open", "{\"a\" 1}", "tru"}) {
            CHECK(Json::parse(bad).kind() == Json::Null);
        }
        CHECK(Json::parse(" [ ] ").kind() == Json::Array && Json::parse("{}").kind() == Json::Object);
        // Numbers as JSON writes them, finite; strtod's other forms are malformed.
        for (const char* bad : {"[nan]", "[inf]", "[-infinity]", "[0x10]", "[1e999]", "[.5]", "[1.]", "[+1]", "[1e]"}) {
            CHECK(Json::parse(bad).kind() == Json::Null);
        }
        CHECK(Json::parse("[-0.5e2]").items()[0].number() == -50);
        // Escapes of four hexadecimal digits; surrogates only in pairs (UTF-8 has no others).
        const std::string u = "\\u";
        for (const std::string& bad : {"[\"" + u + "12\"]", "[\"" + u + " 123\"]", "[\"" + u + "+0x1\"]",
                                       "[\"" + u + "d83dA\"]", "[\"" + u + "d83d" + u + "0041\"]",
                                       "[\"" + u + "de00\"]"}) {
            CHECK(Json::parse(bad.c_str()).kind() == Json::Null);
        }
        CHECK(Json::parse(("[\"" + u + "00e9" + u + "D83D" + u + "DE00\"]").c_str()).items()[0].text() ==
              "\xc3\xa9\xf0\x9f\x98\x80");
        // Arrays and objects maxDepth deep, no deeper: no text runs the parser out of stack.
        const auto nested = [](int depth) {
            return std::string(size_t(depth), '[') + std::string(size_t(depth), ']');
        };
        CHECK(Json::parse(nested(Json::maxDepth).c_str()).kind() == Json::Array);
        CHECK(Json::parse(nested(Json::maxDepth + 1).c_str()).kind() == Json::Null);
        CHECK(Json::parse(std::string(1'000'000, '[').c_str()).kind() == Json::Null);
        CHECK(Json::parse(("{\"a\":" + nested(Json::maxDepth) + "}").c_str()).kind() == Json::Null);
        // A number out of an integer's range is its least or its most, never undefined.
        const Json big = Json::parse("[7.7e65,-7.7e65,4294967296,4294967295.5,1.8446744073709552e19,1e19]");
        CHECK(big.items()[0].u32() == UINT32_MAX && big.items()[0].u64() == UINT64_MAX);
        CHECK(big.items()[1].u32() == 0 && big.items()[1].u64() == 0);
        CHECK(big.items()[2].u32() == UINT32_MAX && big.items()[3].u32() == UINT32_MAX);
        CHECK(big.items()[4].u64() == UINT64_MAX && big.items()[5].u64() == 10'000'000'000'000'000'000ull);
        CHECK(Json::to<int64_t>(-1e300) == INT64_MIN && Json::to<int64_t>(9.3e18) == INT64_MAX &&
              Json::to<int64_t>(-5e6) == -5'000'000 && Json::to<uint64_t>(std::nan("")) == 0);
        // What the glue writes reads back.
        web::JsonWriter inner, outer;
        inner.add("x", int32_t(-5)).add("text", std::string("q\"\\\x01"));
        outer.add("n", uint64_t(18446744073709551615ull)).raw("inner", inner.str()).hex("token", (const uint8_t*)"\x01\xab", 2);
        const Json back = Json::parse(outer.str().c_str());
        CHECK(back["inner"]["x"].number() == -5 && back["inner"]["text"].text() == "q\"\\\x01");
        CHECK(back["token"].text() == "01ab" && back["n"].number() > 1.8e19);
    });
}

#include <gtest/gtest.h>

#include "utils/text_file_buffer.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& suffix) {
    static int seq = 0;
    return fs::temp_directory_path() /
           ("acecode_text_file_buffer_" + std::to_string(++seq) + suffix);
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_bytes(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

std::string bytes(std::initializer_list<unsigned char> values) {
    std::string out;
    for (unsigned char value : values) {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

} // namespace

TEST(TextFileBuffer, DetectsUtf8BomAndNormalizesCrLf) {
    auto decoded = acecode::decode_text_file_bytes(
        bytes({0xEF, 0xBB, 0xBF}) + std::string("a\r\nb\r\n"));

    ASSERT_TRUE(decoded.success) << decoded.error;
    EXPECT_EQ(decoded.buffer.metadata.encoding, acecode::TextEncoding::Utf8Bom);
    EXPECT_TRUE(decoded.buffer.metadata.has_bom);
    EXPECT_EQ(decoded.buffer.metadata.line_ending, acecode::LineEndingStyle::CrLf);
    EXPECT_EQ(decoded.buffer.text, "a\nb\n");

    auto encoded = acecode::encode_text_for_write("a\nc\n", decoded.buffer.metadata);
    ASSERT_TRUE(encoded.success) << encoded.error;
    EXPECT_EQ(encoded.bytes, bytes({0xEF, 0xBB, 0xBF}) + std::string("a\r\nc\r\n"));
}

TEST(TextFileBuffer, DecodesAndEncodesUtf16LittleEndianWithBom) {
    std::string raw = bytes({0xFF, 0xFE, 'a', 0, '\r', 0, '\n', 0, 'b', 0});
    auto decoded = acecode::decode_text_file_bytes(raw);

    ASSERT_TRUE(decoded.success) << decoded.error;
    EXPECT_EQ(decoded.buffer.metadata.encoding, acecode::TextEncoding::Utf16Le);
    EXPECT_TRUE(decoded.buffer.metadata.has_bom);
    EXPECT_EQ(decoded.buffer.text, "a\nb");

    auto encoded = acecode::encode_text_for_write("x\ny", decoded.buffer.metadata);
    ASSERT_TRUE(encoded.success) << encoded.error;
    EXPECT_EQ(encoded.bytes, bytes({0xFF, 0xFE, 'x', 0, '\r', 0, '\n', 0, 'y', 0}));
}

TEST(TextFileBuffer, DecodesAndEncodesUtf16BigEndianWithBom) {
    std::string raw = bytes({0xFE, 0xFF, 0, 'a', 0, '\n', 0, 'b'});
    auto decoded = acecode::decode_text_file_bytes(raw);

    ASSERT_TRUE(decoded.success) << decoded.error;
    EXPECT_EQ(decoded.buffer.metadata.encoding, acecode::TextEncoding::Utf16Be);
    EXPECT_EQ(decoded.buffer.text, "a\nb");

    auto encoded = acecode::encode_text_for_write("c\nb", decoded.buffer.metadata);
    ASSERT_TRUE(encoded.success) << encoded.error;
    EXPECT_EQ(encoded.bytes, bytes({0xFE, 0xFF, 0, 'c', 0, '\n', 0, 'b'}));
}

TEST(TextFileBuffer, RejectsBinaryInput) {
    auto decoded = acecode::decode_text_file_bytes(bytes({0x00, 0x01, 0x02, 0x03, 0x04}));

    EXPECT_FALSE(decoded.success);
    EXPECT_EQ(decoded.buffer.metadata.encoding, acecode::TextEncoding::Binary);
    EXPECT_TRUE(decoded.buffer.metadata.binary);
}

TEST(TextFileBuffer, SafeWriteRollsBackOnVerificationMismatch) {
    auto path = temp_path(".txt");
    write_bytes(path, "before\n");

    auto metadata = acecode::default_new_file_text_metadata();
    metadata.encoding = acecode::TextEncoding::Unsupported;

    auto result = acecode::safe_write_text_file(path.string(), "after\n", metadata);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(read_bytes(path), "before\n");

    fs::remove(path);
}

TEST(TextFileBuffer, RangeHashUsesLfNormalizedContent) {
    const std::string text = "a\nb\nc\n";
    EXPECT_EQ(acecode::line_range_content(text, 2, 3), "b\nc\n");
    EXPECT_FALSE(acecode::range_hash(text, 2, 3).empty());
}

#ifdef _WIN32
TEST(TextFileBuffer, DecodesGbkAndRejectsUnrepresentableText) {
    std::string gbk = bytes({0xD6, 0xD0, 0xCE, 0xC4, '\r', '\n'}); // "中文\r\n" in CP936
    auto decoded = acecode::decode_text_file_bytes(gbk);

    ASSERT_TRUE(decoded.success) << decoded.error;
    EXPECT_EQ(decoded.buffer.metadata.encoding, acecode::TextEncoding::Gbk);
    EXPECT_EQ(decoded.buffer.text, std::string(u8"中文\n"));

    auto ok = acecode::encode_text_for_write(std::string(u8"改动\n"), decoded.buffer.metadata);
    ASSERT_TRUE(ok.success) << ok.error;
    EXPECT_EQ(ok.bytes.find("\xE6\x94\xB9"), std::string::npos);

    auto bad = acecode::encode_text_for_write(std::string(u8"emoji \xF0\x9F\x98\x80\n"),
                                              decoded.buffer.metadata);
    EXPECT_FALSE(bad.success);
}

TEST(TextFileBuffer, DecodesGb18030WhenCp936Fails) {
    // U+20000 in GB18030 is 95 32 82 36; CP936 cannot decode this 4-byte sequence.
    std::string gb18030 = bytes({0x95, 0x32, 0x82, 0x36, '\n'});
    auto decoded = acecode::decode_text_file_bytes(gb18030);

    ASSERT_TRUE(decoded.success) << decoded.error;
    EXPECT_EQ(decoded.buffer.metadata.encoding, acecode::TextEncoding::Gb18030);
    EXPECT_FALSE(decoded.buffer.text.empty());
}
#endif

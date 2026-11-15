// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "core/file_sys/nca_metadata.h"
#include "tests/core/file_sys/metadata_test_file.h"

namespace FileSys {

TEST_CASE("CNMT serializes platform values", "[core][file_sys]") {
    const auto platform = GENERATE(ContentMetaPlatform::Nx, ContentMetaPlatform::Ounce,
                                   ContentMetaPlatform(0x7f));
    const CNMTHeader header{
        .title_id = 0x0100000000000000,
        .type = TitleType::Application,
        .reserved = u8(platform),
        .table_offset = sizeof(OptionalHeader),
    };
    CNMT metadata(header, {}, {}, {});

    const auto serialized = metadata.Serialize();
    REQUIRE(serialized[0xD] == metadata.GetHeader().reserved);
    CNMT parsed(std::make_shared<Tests::MetadataFile>(serialized));
    REQUIRE(parsed.GetPlatform() == platform);
    REQUIRE(parsed.Serialize() == serialized);
}

TEST_CASE("CNMT reads meta records after content records", "[core][file_sys]") {
    const auto content_count = GENERATE(as<u16>{}, 0, 1, 2);
    const CNMTHeader header{
        .title_id = 0x0100000000000000,
        .type = TitleType::Application,
        .table_offset = sizeof(OptionalHeader),
        .number_content_entries = content_count,
        .number_meta_entries = 2,
    };
    std::vector<ContentRecord> contents(content_count);
    for (u16 i = 0; i < content_count; ++i) {
        contents[i].hash[0] = u8(0xAA + i);
        contents[i].nca_id[0] = u8(i + 1);
    }
    const std::vector<MetaRecord> records{
        {.title_id = 0x0100000000000800, .title_version = 7, .type = TitleType::Update},
        {.title_id = 0x0100000000001001, .title_version = 3, .type = TitleType::AOC},
    };
    const CNMT metadata(header, {}, contents, records);
    const CNMT parsed(std::make_shared<Tests::MetadataFile>(metadata.Serialize()));
    REQUIRE(parsed.GetContentRecords().size() == contents.size());
    REQUIRE(parsed.GetMetaRecords().size() == records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        REQUIRE(parsed.GetMetaRecords()[i].title_id == records[i].title_id);
        REQUIRE(parsed.GetMetaRecords()[i].title_version == records[i].title_version);
        REQUIRE(parsed.GetMetaRecords()[i].type == records[i].type);
    }
}

TEST_CASE("CNMT does not merge platforms", "[core][file_sys]") {
    CNMTHeader nx_header{};
    nx_header.type = TitleType::Application;
    nx_header.reserved = u8(ContentMetaPlatform::Nx);
    nx_header.table_offset = sizeof(OptionalHeader);
    auto ounce_header = nx_header;
    ounce_header.reserved = u8(ContentMetaPlatform::Ounce);
    ounce_header.number_content_entries = 1;

    ContentRecord content{};
    content.nca_id[0] = 1;
    CNMT nx(nx_header, {}, {}, {});
    CNMT ounce(ounce_header, {}, {content}, {});
    REQUIRE_FALSE(nx.UnionRecords(ounce));
    REQUIRE(nx.GetContentRecords().empty());

    auto target_header = ounce_header;
    target_header.number_content_entries = 0;
    CNMT ounce_target(target_header, {}, {}, {});
    REQUIRE(ounce_target.UnionRecords(ounce));
    REQUIRE(ounce_target.GetContentRecords().size() == 1);
}

} // namespace FileSys

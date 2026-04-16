#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/init_embeddings.hpp>

using namespace tiny_agent;

// ═══════════════════════════════════════════════════════════════════════════
// parse_embedding_model_string — explicit provider:model
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("parse explicit openai") {
    auto s = parse_embedding_model_string("openai:text-embedding-3-small");
    CHECK(s.provider == "openai");
    CHECK(s.model    == "text-embedding-3-small");
}

TEST_CASE("parse explicit gemini") {
    auto s = parse_embedding_model_string("gemini:text-embedding-004");
    CHECK(s.provider == "gemini");
    CHECK(s.model    == "text-embedding-004");
}

TEST_CASE("parse explicit mistral") {
    auto s = parse_embedding_model_string("mistral:mistral-embed");
    CHECK(s.provider == "mistral");
    CHECK(s.model    == "mistral-embed");
}

TEST_CASE("parse explicit cohere") {
    auto s = parse_embedding_model_string("cohere:embed-v4");
    CHECK(s.provider == "cohere");
    CHECK(s.model    == "embed-v4");
}

TEST_CASE("parse explicit voyageai") {
    auto s = parse_embedding_model_string("voyageai:voyage-4");
    CHECK(s.provider == "voyageai");
    CHECK(s.model    == "voyage-4");
}

// ═══════════════════════════════════════════════════════════════════════════
// parse_embedding_model_string — auto-detection
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("auto-detect openai from text-embedding- prefix") {
    CHECK(parse_embedding_model_string("text-embedding-3-small").provider == "openai");
    CHECK(parse_embedding_model_string("text-embedding-3-large").provider == "openai");
    CHECK(parse_embedding_model_string("text-embedding-ada-002").provider == "openai");
}

TEST_CASE("auto-detect gemini from embedding- prefix") {
    CHECK(parse_embedding_model_string("embedding-001").provider == "gemini");
}

TEST_CASE("auto-detect gemini from text-multilingual- prefix") {
    CHECK(parse_embedding_model_string("text-multilingual-embedding-002").provider == "gemini");
}

TEST_CASE("auto-detect mistral from mistral-embed") {
    CHECK(parse_embedding_model_string("mistral-embed").provider == "mistral");
}

TEST_CASE("auto-detect cohere from embed- prefix") {
    CHECK(parse_embedding_model_string("embed-v4").provider == "cohere");
    CHECK(parse_embedding_model_string("embed-multilingual-v3.0").provider == "cohere");
    CHECK(parse_embedding_model_string("embed-english-v3.0").provider == "cohere");
}

TEST_CASE("auto-detect voyageai from voyage- prefix") {
    CHECK(parse_embedding_model_string("voyage-4").provider        == "voyageai");
    CHECK(parse_embedding_model_string("voyage-4-large").provider  == "voyageai");
    CHECK(parse_embedding_model_string("voyage-4-lite").provider   == "voyageai");
    CHECK(parse_embedding_model_string("voyage-code-3").provider   == "voyageai");
}

TEST_CASE("unknown model defaults to openai") {
    auto s = parse_embedding_model_string("my-custom-model");
    CHECK(s.provider == "openai");
    CHECK(s.model    == "my-custom-model");
}

TEST_CASE("edge: empty string") {
    auto s = parse_embedding_model_string("");
    CHECK(s.provider == "openai");
    CHECK(s.model.empty());
}

TEST_CASE("edge: colon-only is treated as non-provider") {
    auto s = parse_embedding_model_string(":");
    CHECK(s.provider == "openai");
    CHECK(s.model    == ":");
}

// ═══════════════════════════════════════════════════════════════════════════
// init_embeddings — factory construction (offline, no HTTP calls)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("init_embeddings returns AnyEmbeddings (openai)") {
    auto emb = init_embeddings("openai:text-embedding-3-small",
                               EmbeddingConfig{.api_key = "fake-key"});
    CHECK(emb.model_name() == "text-embedding-3-small");
}

TEST_CASE("init_embeddings returns AnyEmbeddings (gemini)") {
    auto emb = init_embeddings("gemini:text-embedding-004",
                               EmbeddingConfig{.api_key = "fake-key"});
    CHECK(emb.model_name() == "text-embedding-004");
}

TEST_CASE("init_embeddings returns AnyEmbeddings (mistral)") {
    auto emb = init_embeddings("mistral:mistral-embed",
                               EmbeddingConfig{.api_key = "fake-key"});
    CHECK(emb.model_name() == "mistral-embed");
}

TEST_CASE("init_embeddings returns AnyEmbeddings (cohere)") {
    auto emb = init_embeddings("cohere:embed-v4",
                               EmbeddingConfig{.api_key = "fake-key"});
    CHECK(emb.model_name() == "embed-v4");
}

TEST_CASE("init_embeddings returns AnyEmbeddings (voyageai)") {
    auto emb = init_embeddings("voyageai:voyage-4",
                               EmbeddingConfig{.api_key = "fake-key"});
    CHECK(emb.model_name() == "voyage-4");
}

TEST_CASE("init_embeddings auto-detect") {
    auto emb = init_embeddings("text-embedding-3-small",
                               EmbeddingConfig{.api_key = "fake"});
    CHECK(emb.model_name() == "text-embedding-3-small");
}

TEST_CASE("init_embeddings unknown provider throws") {
    CHECK_THROWS_AS(
        init_embeddings("unknownprovider:model", EmbeddingConfig{.api_key = "x"}),
        Error);
}

TEST_CASE("init_embeddings two-arg overload") {
    auto emb = init_embeddings("openai", "text-embedding-3-small",
                               EmbeddingConfig{.api_key = "fake"});
    CHECK(emb.model_name() == "text-embedding-3-small");
}

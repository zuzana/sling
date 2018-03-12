// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Utility tool for generating a fully populated master spec.
// In particular, it creates the action table, all resources needed by the
// features, computes the feature domain sizes and uses all this to output
// a full MasterSpec.
//
// Input arguments:
// - Path to a commons store.
// - File pattern of training documents.
// - Name of output directory.
//
// Sample usage:
//   bazel-bin/nlp/parser/trainer/generate-master-spec
//       --documents='/tmp/documents.*'
//       --commons=/tmp/common_store.encoded
//       --output_dir='/tmp/out'

#include <string>
#include <vector>

#include "sling/base/flags.h"
#include "sling/base/init.h"
#include "sling/base/logging.h"
#include "sling/base/macros.h"
#include "sling/file/file.h"
#include "sling/frame/object.h"
#include "sling/frame/serialization.h"
#include "sling/frame/store.h"
#include "sling/nlp/document/affix.h"
#include "sling/nlp/document/document.h"
#include "sling/nlp/document/features.h"
#include "sling/nlp/document/document-source.h"
#include "sling/nlp/parser/trainer/action-table-generator.h"
#include "sling/nlp/parser/trainer/shared-resources.h"
#include "sling/stream/file.h"
#include "sling/string/strcat.h"
#include "sling/util/embeddings.h"

using sling::File;
using sling::FileDecoder;
using sling::Object;
using sling::Store;
using sling::StrAppend;
using sling::StrCat;
using sling::EmbeddingReader;
using sling::nlp::ActionTable;
using sling::nlp::AffixTable;
using sling::nlp::ActionTableGenerator;
using sling::nlp::SharedResources;
using sling::nlp::Document;
using sling::nlp::DocumentFeatures;
using sling::nlp::DocumentSource;

using syntaxnet::dragnn::ComponentSpec;
using syntaxnet::dragnn::MasterSpec;
using syntaxnet::dragnn::RegisteredModuleSpec;

DEFINE_string(documents, "", "File pattern of training documents.");
DEFINE_string(commons, "", "Path to common store.");
DEFINE_string(output_dir, "", "Output directory.");
DEFINE_int32(word_embeddings_dim, 32, "Word embeddings dimensionality.");
DEFINE_string(word_embeddings, "",
              "TF recordio of pretrained word embeddings. Should have a "
              "dimensionality = FLAGS_word_embeddings_dim.");
DEFINE_bool(oov_lstm_features, true,
            "Whether fallback features (shape, suffix etc) should "
            "be used in the LSTMs");

// Various options for generating the action table, lexicons, spec.
constexpr int kActionTableCoveragePercentile = 99;
constexpr bool kActionTableFromPerSentence = true;
constexpr int kMaxPrefixLength = 3;
constexpr int kMaxSuffixLength = 3;

// Maximum number of ids generated by a role feature.
constexpr int kMaxRoleIds = 32;

// Frame limit for role features.
constexpr int kRoleFrameLimit = 5;

// Workspace for various artifacts that are used/created by this tool.
struct Artifacts {
  SharedResources resources;

  DocumentSource *train_corpus = nullptr;  // training corpus
  string commons_filename;                 // path to commons
  string action_table_filename;            // path of generated action table

  // Filenames of generated lexical resources.
  string prefix_table;
  string suffix_table;
  string word_vocab;

  // Number of entries of various lexical varieties.
  int num_words = 0;
  int num_prefixes = 0;
  int num_suffixes = 0;

  // OOV word id.
  int lexicon_oov = 0;

  // Generated master spec and its file.
  MasterSpec spec;
  string spec_file;

  ~Artifacts() { delete train_corpus; }
  Store *global() { return resources.global; }
  const ActionTable &table() { return resources.table; }
};

// Returns the full output path for 'basename'.
string FullName(const string &basename) {
  string s = FLAGS_output_dir;
  CHECK(!s.empty());
  if (s.back() == '/') s.pop_back();
  return sling::StrCat(s, "/", basename);
}

void OutputActionTable(Artifacts *artifacts) {
  ActionTableGenerator generator(artifacts->global());
  generator.set_coverage_percentile(kActionTableCoveragePercentile);
  generator.set_per_sentence(kActionTableFromPerSentence);

  int count = 0;
  while (true) {
    Store store(artifacts->global());
    Document *document = artifacts->train_corpus->Next(&store);
    if (document == nullptr) break;

    count++;
    generator.Add(*document);
    if (count % 10000 == 0) LOG(INFO) << count << " documents processed.";
    delete document;
  }
  LOG(INFO) << "Processed " << count << " documents.";

  string table_file = FullName("table");
  string summary_file = FullName("table.summary");
  string unknown_file = FullName("table.unknown_symbols");
  generator.Save(table_file, summary_file, unknown_file);
  artifacts->action_table_filename = table_file;

  LOG(INFO) << "Wrote action table to " << table_file
            << ", " << summary_file << ", " << unknown_file;
  artifacts->resources.LoadActionTable(table_file);
}

syntaxnet::dragnn::ComponentSpec *AddComponent(
    const string &name,
    const string &backend,
    const string &network_unit,
    const string &transition_system,
    Artifacts *artifacts) {
  auto *c = artifacts->spec.add_component();
  c->set_name(name);
  c->mutable_backend()->set_registered_name(backend);
  c->mutable_network_unit()->set_registered_name(network_unit);
  c->mutable_transition_system()->set_registered_name(transition_system);
  c->mutable_component_builder()->set_registered_name(
      "DynamicComponentBuilder");
  return c;
}

void SetParam(RegisteredModuleSpec *spec,
              const string &key,
              const string &value) {
  (*spec->mutable_parameters())[key] = value;
}

void AddFixedFeature(ComponentSpec *component,
                     const string &name,
                     const string &feature,
                     const string &arg,
                     int embedding_dim,
                     int vocab_size,
                     int max_num_ids = 1) {
  auto *f = component->add_fixed_feature();
  f->set_name(name);
  f->set_fml(feature + (arg.empty() ? "" : " ") + arg);
  f->set_embedding_dim(embedding_dim);
  f->set_vocabulary_size(vocab_size);
  f->set_size(max_num_ids);
}

void AddFixedFeature(ComponentSpec *component,
                     const string &name,
                     const string &feature,
                     int embedding_dim,
                     int vocab_size,
                     int max_num_ids = 1) {
  AddFixedFeature(
      component, name, feature, "", embedding_dim, vocab_size, max_num_ids);
}

void AddLinkedFeature(ComponentSpec *component,
                      const string &name,
                      const string &feature,
                      int max,
                      int embedding_dim,
                      const string &source,
                      const string &translator) {
  auto *f = component->add_linked_feature();
  f->set_name(name);
  f->set_fml(feature);
  f->set_embedding_dim(embedding_dim);
  f->set_source_component(source);
  f->set_source_translator(translator);
  f->set_source_layer("layer_0");
  f->set_size(max);
}

void AddLinkedFeature(ComponentSpec *component,
                      const string &name,
                      const string &feature,
                      int max,
                      int embedding_dim,
                      const string &source) {
  AddLinkedFeature(
      component, name, feature, max, embedding_dim, source, "identity");
}

void AddResource(ComponentSpec *spec,
                 const string &name,
                 const string &file_pattern) {
  auto *r = spec->add_resource();
  r->set_name(name);
  r->add_part()->set_file_pattern(file_pattern);
}

// Writes an affix table to 'output_file'.
void WriteAffixTable(AffixTable *affixes, const string &output_file) {
  sling::FileOutputStream stream(output_file);
  affixes->Write(&stream);
  CHECK(stream.Close());
}

void OutputResources(Artifacts *artifacts) {
  // Affix tables to be populated by the corpus.
  AffixTable prefixes(AffixTable::PREFIX, kMaxPrefixLength);
  AffixTable suffixes(AffixTable::SUFFIX, kMaxSuffixLength);

  int count = 0;
  artifacts->train_corpus->Rewind();

  std::unordered_set<string> words;
  std::vector<string> id_to_word;
  words.insert("<UNKNOWN>");
  id_to_word.emplace_back("<UNKNOWN>");
  artifacts->lexicon_oov = 0;
  while (true) {
    Store store(artifacts->global());
    Document *document = artifacts->train_corpus->Next(&store);
    if (document == nullptr) break;

    for (int t = 0; t < document->num_tokens(); ++t) {
      const auto &token = document->token(t);
      string word = token.text();
      for (char &c : word) {
        if (c >= '0' && c <= '9') c = '9';
      }

      if (words.find(word) == words.end()) {
        id_to_word.emplace_back(word);
        words.insert(word);
      }

      // Add prefixes/suffixes for the current word.
      prefixes.AddAffixesForWord(token.text());
      suffixes.AddAffixesForWord(token.text());
    }

    if (++count % 10000 == 0) {
      LOG(INFO) << count << " documents processsed while building lexicons";
    }
    delete document;
  }

  // Write affixes to disk.
  artifacts->num_prefixes = prefixes.size();
  artifacts->num_suffixes = suffixes.size();
  artifacts->prefix_table = FullName("prefix-table");
  artifacts->suffix_table = FullName("suffix-table");
  WriteAffixTable(&prefixes, artifacts->prefix_table);
  WriteAffixTable(&suffixes, artifacts->suffix_table);

  // Write word vocabulary.
  artifacts->word_vocab = FullName("word-vocab");
  artifacts->num_words = id_to_word.size();
  string contents;
  for (const string &w : id_to_word) {
    StrAppend(&contents, !contents.empty() ?  "\n" : "", w);
  }
  CHECK(File::WriteContents(artifacts->word_vocab, contents));

  LOG(INFO) << count << " documents processsed while building lexicon";
}

void CheckWordEmbeddingsDimensionality() {
  if (FLAGS_word_embeddings.empty()) return;

  EmbeddingReader reader(FLAGS_word_embeddings);
  CHECK_EQ(reader.dim(), FLAGS_word_embeddings_dim)
      << "Pretrained embeddings have dim=" << reader.dim()
      << ", but specified word embedding dim=" << FLAGS_word_embeddings_dim;
}

void OutputMasterSpec(Artifacts *artifacts) {
  CheckWordEmbeddingsDimensionality();

  // Left to right LSTM.
  auto *lr_lstm = AddComponent(
      "lr_lstm", "SemparComponent", "LSTMNetwork", "shift-only", artifacts);
  auto *system = lr_lstm->mutable_transition_system();
  SetParam(system, "left_to_right", "true");
  SetParam(system, "lexicon_oov", StrCat(artifacts->lexicon_oov));
  SetParam(system, "lexicon_normalize_digits", "true");
  SetParam(lr_lstm->mutable_network_unit(), "hidden_layer_sizes", "256");
  lr_lstm->set_num_actions(1);
  AddResource(lr_lstm, "commons", artifacts->commons_filename);

  AddFixedFeature(
      lr_lstm, "words", "word", FLAGS_word_embeddings_dim,
      artifacts->num_words);
  AddResource(lr_lstm, "word-vocab", artifacts->word_vocab);

  if (FLAGS_oov_lstm_features) {
    AddFixedFeature(
        lr_lstm, "suffix", "suffix", 16, artifacts->num_suffixes,
        kMaxSuffixLength);
    AddResource(lr_lstm, "suffix-table", artifacts->suffix_table);
    AddFixedFeature(lr_lstm, "capitalization", "capitalization", 8,
                    DocumentFeatures::CAPITALIZATION_CARDINALITY);
    AddFixedFeature(lr_lstm, "hyphen", "hyphen", 8,
                    DocumentFeatures::HYPHEN_CARDINALITY);
    AddFixedFeature(lr_lstm, "punctuation", "punctuation", 8,
                    DocumentFeatures::PUNCTUATION_CARDINALITY);
    AddFixedFeature(lr_lstm, "quote", "quote", 8,
                    DocumentFeatures::QUOTE_CARDINALITY);
    AddFixedFeature(lr_lstm, "digit", "digit", 8,
                    DocumentFeatures::DIGIT__CARDINALITY);
  }

  // Right to left LSTM.
  auto *rl_lstm = artifacts->spec.add_component();
  *rl_lstm = *lr_lstm;
  rl_lstm->set_name("rl_lstm");
  SetParam(rl_lstm->mutable_transition_system(), "left_to_right", "false");

  // Feed forward unit.
  auto *ff = AddComponent(
      "ff", "SemparComponent", "FeedForwardNetwork", "sempar", artifacts);
  SetParam(ff->mutable_network_unit(), "hidden_layer_sizes", "128");
  ff->set_num_actions(artifacts->table().NumActions());

  string arg = StrCat(kRoleFrameLimit);
  int roles = artifacts->resources.roles.size();
  int fr = roles * kRoleFrameLimit;
  int f2 = kRoleFrameLimit * kRoleFrameLimit;
  if (fr > 0) {
    AddFixedFeature(ff, "in-roles", "in-roles", arg, 16, fr, kMaxRoleIds);
    AddFixedFeature(ff, "out-roles", "out-roles", arg, 16, fr, kMaxRoleIds);
    AddFixedFeature(
        ff, "labeled-roles", "labeled-roles", arg, 16, f2 * roles, kMaxRoleIds);
    AddFixedFeature(
        ff, "unlabeled-roles", "unlabeled-roles", arg, 16, f2, kMaxRoleIds);
  }

  AddLinkedFeature(ff, "frame-creation-steps", "frame-creation", 5, 64, "ff");
  AddLinkedFeature(ff, "frame-focus-steps", "frame-focus", 5, 64, "ff");
  AddLinkedFeature(ff, "frame-end-lr", "frame-end", 5, 32, "lr_lstm");
  AddLinkedFeature(
      ff, "frame-end-rl", "frame-end", 5, 32, "rl_lstm", "reverse-token");
  AddLinkedFeature(ff, "history", "history", 4, 64, "ff", "history");
  AddLinkedFeature(ff, "lr", "focus", 1, 32, "lr_lstm");
  AddLinkedFeature(ff, "rl", "focus", 1, 32, "rl_lstm", "reverse-token");

  // Add any resources required by the feed forward unit's features.
  AddResource(ff, "commons", artifacts->commons_filename);
  AddResource(ff, "action-table", artifacts->action_table_filename);

  // Add pretrained embeddings for word features.
  if (!FLAGS_word_embeddings.empty()) {
    for (auto &component : *artifacts->spec.mutable_component()) {
      string vocab_file;
      for (const auto &resource : component.resource()) {
        if (resource.name() == "word-vocab") {
          vocab_file = resource.part(0).file_pattern();
        }
      }
      if (!vocab_file.empty()) {
        for (auto &feature : *component.mutable_fixed_feature()) {
          if (feature.name() == "words") {
            feature.mutable_pretrained_embedding_matrix()->add_part()->
                set_file_pattern(FLAGS_word_embeddings);
            feature.mutable_vocab()->add_part()->set_file_pattern(vocab_file);
          }
        }
      }
    }
    LOG(INFO) << "Using pretrained word embeddings: " << FLAGS_word_embeddings;
  } else {
    LOG(INFO) << "No pretrained word embeddings specified";
  }

  // Dump the master spec.
  string spec_file = FullName("master_spec");
  CHECK(File::WriteContents(spec_file, artifacts->spec.DebugString()));
  artifacts->spec_file = spec_file;
  LOG(INFO) << "Wrote master spec to " << spec_file;
}

int main(int argc, char **argv) {
  sling::InitProgram(&argc, &argv);

  CHECK(!FLAGS_documents.empty()) << "No documents specified.";
  CHECK(!FLAGS_commons.empty()) << "No commons specified.";
  CHECK(!FLAGS_output_dir.empty()) << "No output_dir specified.";

  if (!File::Exists(FLAGS_output_dir)) {
    CHECK(File::Mkdir(FLAGS_output_dir));
  }

  Artifacts artifacts;
  artifacts.commons_filename = FLAGS_commons;
  artifacts.resources.LoadGlobalStore(FLAGS_commons);
  artifacts.train_corpus = DocumentSource::Create(FLAGS_documents);

  // Dump action table.
  OutputActionTable(&artifacts);

  // Output lexical resources.
  OutputResources(&artifacts);

  // Make master spec.
  OutputMasterSpec(&artifacts);

  return 0;
}

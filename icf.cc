#include "mold.h"

#include <array>
#include <openssl/sha.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>

static constexpr i64 HASH_SIZE = 16;

typedef std::array<u8, HASH_SIZE> Digest;

static bool is_eligible(InputSection &isec) {
  bool is_alloc = (isec.shdr.sh_flags & SHF_ALLOC);
  bool is_executable = (isec.shdr.sh_flags & SHF_EXECINSTR);
  bool is_writable = (isec.shdr.sh_flags & SHF_WRITE);
  bool is_bss = (isec.shdr.sh_type == SHT_NOBITS);
  bool is_init = (isec.shdr.sh_type == SHT_INIT_ARRAY || isec.name == ".init");
  bool is_fini = (isec.shdr.sh_type == SHT_FINI_ARRAY || isec.name == ".fini");
  bool is_enumerable = is_c_identifier(isec.name);

  return is_alloc && is_executable && !is_writable && !is_bss &&
         !is_init && !is_fini && !is_enumerable;
}

static Digest digest_final(SHA256_CTX &ctx) {
  u8 digest[SHA256_SIZE];
  assert(SHA256_Final(digest, &ctx) == 1);

  Digest arr;
  memcpy(arr.data(), digest, HASH_SIZE);
  return arr;
}

static Digest compute_digest(InputSection &isec) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);

  auto hash_i64 = [&](i64 val) {
    SHA256_Update(&ctx, &val, 8);
  };

  auto hash_string = [&](std::string_view str) {
    hash_i64(str.size());
    SHA256_Update(&ctx, str.data(), str.size());
  };

  auto hash_symbol = [&](Symbol &sym) {
    if (SectionFragment *frag = sym.frag) {
      hash_i64(2);
      hash_string(frag->data);
    } else if (!sym.input_section) {
      hash_i64(3);
    } else if (!sym.input_section->icf_eligible) {
      hash_i64(4);
      hash_i64(sym.input_section->icf_idx);
    } else {
      hash_i64(5);
    }
    hash_i64(sym.value);
  };

  hash_string(isec.get_contents());
  hash_i64(isec.shdr.sh_flags);
  hash_i64(isec.fdes.size());
  hash_i64(isec.rels.size());

  for (FdeRecord &fde : isec.fdes) {
    // Bytes 4 to 8 contain an offset to CIE
    hash_string(fde.contents.substr(0, 4));
    hash_string(fde.contents.substr(8));

    hash_i64(fde.rels.size());

    for (EhReloc &rel : std::span(fde.rels).subspan(1)) {
      hash_symbol(rel.sym);
      hash_i64(rel.type);
      hash_i64(rel.offset);
      hash_i64(rel.addend);
    }
  }

  i64 ref_idx = 0;

  for (i64 i = 0; i < isec.rels.size(); i++) {
    ElfRela &rel = isec.rels[i];
    hash_i64(rel.r_offset);
    hash_i64(rel.r_type);
    hash_i64(rel.r_addend);

    if (isec.has_fragments[i]) {
      SectionFragmentRef &ref = isec.rel_fragments[ref_idx++];
      hash_i64(1);
      hash_i64(ref.addend);
      hash_string(ref.frag->data);
    } else {
      hash_symbol(*isec.file->symbols[rel.r_sym]);
    }
  }

  return digest_final(ctx);
}

static Digest pack_number(i64 val) {
  Digest arr;
  memset(arr.data(), 0, HASH_SIZE);
  memcpy(arr.data(), &val, 8);
  return arr;
}

static std::vector<InputSection *> gather_sections() {
  Timer t("gather");

  // Count the number of input sections for each input file.
  std::vector<i64> num_sections(out::objs.size());

  tbb::parallel_for((i64)0, (i64)out::objs.size(), [&](i64 i) {
    for (InputSection *isec : out::objs[i]->sections)
      if (isec)
        num_sections[i]++;
  });

  std::vector<i64> section_indices(out::objs.size());
  for (i64 i = 0; i < out::objs.size() - 1; i++)
    section_indices[i + 1] = section_indices[i] + num_sections[i];

  std::vector<InputSection *> sections(section_indices.back() + num_sections.back());

  // Fill `sections` contents.
  tbb::parallel_for((i64)0, (i64)out::objs.size(), [&](i64 i) {
    i64 idx = section_indices[i];
    for (i64 j = 0; j < out::objs[i]->sections.size(); j++)
      if (InputSection *isec = out::objs[i]->sections[j])
        sections[idx++] = isec;
  });

  tbb::enumerable_thread_specific<i64> num_eligibles;

  tbb::parallel_for_each(sections.begin(), sections.end(), [&](InputSection *isec) {
    if (is_eligible(*isec)) {
      isec->icf_eligible = true;
      num_eligibles.local() += 1;
    }
  });

  tbb::parallel_sort(sections.begin(), sections.end(),
                     [](InputSection *a, InputSection *b) {
                       if (a->icf_eligible ^ b->icf_eligible)
                         return a->icf_eligible && !b->icf_eligible;
                       return a->get_priority() < b->get_priority();
                     });

  tbb::parallel_for((i64)0, (i64)sections.size(), [&](i64 i) {
    sections[i]->icf_idx = i;
  });

  sections.resize(num_eligibles.combine(std::plus()));
  return sections;
}

static std::vector<Digest> compute_digests(std::span<InputSection *> sections) {
  Timer t("compute_digests");

  std::vector<Digest> digests(sections.size());
  tbb::parallel_for((i64)0, (i64)sections.size(), [&](i64 i) {
    digests[i] = compute_digest(*sections[i]);
  });
  return digests;
}

static void gather_edges(std::span<InputSection *> sections,
                         std::vector<u32> &edges, std::vector<u32> &edge_indices) {
  Timer t("gather_edges");
}

void icf_sections() {
  Timer t("icf");

  // Prepare for the propagation rounds.
  std::vector<InputSection *> sections = gather_sections();
  std::vector<Digest> digests0 = compute_digests(sections);

  std::vector<u32> edge_indices;
  std::vector<u32> edges;
  gather_edges(sections, edges, edge_indices);

  return;

  std::vector<std::vector<Digest>> digests(2);
  digests[0] = std::move(digests0);
  digests[1] = digests[0];

  i64 slot = 0;

  auto count_num_classes = [&]() {
    Timer t("count");
    tbb::enumerable_thread_specific<i64> num_classes;
    tbb::parallel_for((i64)0, (i64)sections.size() - 1, [&](i64 i) {
      if (digests[slot][i] != digests[slot][i + 1])
        num_classes.local()++;
    });
    return num_classes.combine(std::plus());
  };

  i64 num_classes = count_num_classes();
  // SyncOut() << "num_classes=" << num_classes;

  Timer t2("propagate");
  static Counter round("icf_round");

  // Execute the propagation rounds until convergence is obtained.
  for (;;) {
    Timer t("round");
    round.inc();

    tbb::parallel_for((i64)0, (i64)sections.size(), [&](i64 i) {
      SHA256_CTX ctx;
      SHA256_Init(&ctx);
      SHA256_Update(&ctx, digests[slot][i].data(), HASH_SIZE);

      i64 begin = edge_indices[i];
      i64 end = (i + 1 == sections.size()) ? edges.size() : edge_indices[i + 1];
      for (i64 j = begin; j < end; j++)
        SHA256_Update(&ctx, digests[slot][edges[j]].data(), HASH_SIZE);

      digests[slot ^ 1][i] = digest_final(ctx);
    });

    slot ^= 1;

    i64 n = count_num_classes();
    if (n == num_classes)
      break;
    num_classes = n;
  }
  t2.stop();


  // Group sections by SHA1 digest.
  Timer t3("merge");

  struct Entry {
    InputSection *isec;
    Digest digest;
  };

  std::vector<Entry> entries;
  entries.resize(sections.size());

  tbb::parallel_for((i64)0, (i64)sections.size(), [&](i64 i) {
    entries[i] = {sections[i], digests[slot][i]};
  });

  {
    Timer t("sort");
    tbb::parallel_sort(entries.begin(), entries.end(), [](auto &a, auto &b) {
      if (a.digest != b.digest)
        return a.digest < b.digest;
      return a.isec->get_priority() < b.isec->get_priority();
    });
  }

  tbb::parallel_for((i64)0, (i64)entries.size() - 1, [&](i64 i) {
    if (i == 0 || entries[i - 1].digest != entries[i].digest) {
      InputSection *leader = entries[i].isec;
      i64 j = i + 1;
      while (j < entries.size() && entries[i].digest == entries[j].digest)
        entries[j++].isec->leader = leader;
    }
  });

  // Re-assign input sections to symbols.
  tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
    for (Symbol *sym : file->symbols) {
      if (sym->input_section && sym->input_section->leader)
        sym->input_section = sym->input_section->leader;
    }
  });

  tbb::parallel_for_each(entries, [&](Entry &ent) {
    InputSection &isec = *ent.isec;
    if (isec.leader)
      isec.kill();
  });

  if (config.print_icf_sections) {
    i64 saved_bytes = 0;

    for (i64 i = 0; i < entries.size(); i++) {
      i64 j = i + 1;
      while (j < entries.size() && entries[i].isec == entries[j].isec->leader)
        j++;

      if (j != i + 1) {
        SyncOut() << "selected section " << *entries[i].isec;
        for (int k = i + 1; k < j; k++)
          SyncOut() << "  removing identical section " << *entries[k].isec;
        saved_bytes += entries[i].isec->get_contents().size() * (j - i - 1);
      }
    }

    SyncOut() << "ICF saved " << saved_bytes << " bytes";
  }
}

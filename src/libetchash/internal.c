/*
  This file is part of etchash.

  etchash is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  etchash is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cpp-ethereum.	If not, see <http://www.gnu.org/licenses/>.
*/
/** @file internal.c
* @author Tim Hughes <tim@twistedfury.com>
* @author Matthew Wampler-Doty
* @date 2015
*/

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <errno.h>
#include <math.h>
#include "mmap.h"
#include "etchash.h"
#include "fnv.h"
#include "endian.h"
#include "internal.h"
#include "data_sizes.h"
#include "io.h"

#ifdef WITH_CRYPTOPP

#include "sha3_cryptopp.h"

#else
#include "sha3.h"
#endif // WITH_CRYPTOPP

uint64_t get_epoch_number(uint64_t const block_number)
{
    uint64_t epoch_length = ETCHASH_EPOCH_LENGTH;
    if (block_number >= ETCHASH_ACTIVATION_BLOCK) {
        epoch_length = ETCHASH_NEW_EPOCH_LENGTH;
    }
    return block_number / epoch_length;
}

uint64_t etchash_get_datasize(uint64_t const block_number)
{
	uint64_t const epoch_number = get_epoch_number(block_number);
	assert(epoch_number < 2048);
	return dag_sizes[epoch_number];
}

uint64_t etchash_get_cachesize(uint64_t const block_number)
{
	uint64_t const epoch_number = get_epoch_number(block_number);
	assert(epoch_number < 2048);
	return cache_sizes[epoch_number];
}

// Follows Sergio's "STRICT MEMORY HARD HASHING FUNCTIONS" (2014)
// https://bitslog.files.wordpress.com/2013/12/memohash-v0-3.pdf
// SeqMemoHash(s, R, N)
bool static etchash_compute_cache_nodes(
	node* const nodes,
	uint64_t cache_size,
	etchash_h256_t const* seed
)
{
	if (cache_size % sizeof(node) != 0) {
		return false;
	}
	uint32_t const num_nodes = (uint32_t) (cache_size / sizeof(node));

	SHA3_512(nodes[0].bytes, (uint8_t*)seed, 32);

	for (uint32_t i = 1; i != num_nodes; ++i) {
		SHA3_512(nodes[i].bytes, nodes[i - 1].bytes, 64);
	}

	for (uint32_t j = 0; j != ETCHASH_CACHE_ROUNDS; j++) {
		for (uint32_t i = 0; i != num_nodes; i++) {
			uint32_t const idx = nodes[i].words[0] % num_nodes;
			node data;
			data = nodes[(num_nodes - 1 + i) % num_nodes];
			for (uint32_t w = 0; w != NODE_WORDS; ++w) {
				data.words[w] ^= nodes[idx].words[w];
			}
			SHA3_512(nodes[i].bytes, data.bytes, sizeof(data));
		}
	}

	// now perform endian conversion
	fix_endian_arr32(nodes->words, num_nodes * NODE_WORDS);
	return true;
}

void etchash_calculate_dag_item(
	node* const ret,
	uint32_t node_index,
	etchash_light_t const light
)
{
	uint32_t num_parent_nodes = (uint32_t) (light->cache_size / sizeof(node));
	node const* cache_nodes = (node const *) light->cache;
	node const* init = &cache_nodes[node_index % num_parent_nodes];
	memcpy(ret, init, sizeof(node));
	ret->words[0] ^= node_index;
	SHA3_512(ret->bytes, ret->bytes, sizeof(node));
#if defined(_M_X64) && ENABLE_SSE
	__m128i const fnv_prime = _mm_set1_epi32(FNV_PRIME);
	__m128i xmm0 = ret->xmm[0];
	__m128i xmm1 = ret->xmm[1];
	__m128i xmm2 = ret->xmm[2];
	__m128i xmm3 = ret->xmm[3];
#endif

	for (uint32_t i = 0; i != ETCHASH_DATASET_PARENTS; ++i) {
		uint32_t parent_index = fnv_hash(node_index ^ i, ret->words[i % NODE_WORDS]) % num_parent_nodes;
		node const *parent = &cache_nodes[parent_index];

#if defined(_M_X64) && ENABLE_SSE
		{
			xmm0 = _mm_mullo_epi32(xmm0, fnv_prime);
			xmm1 = _mm_mullo_epi32(xmm1, fnv_prime);
			xmm2 = _mm_mullo_epi32(xmm2, fnv_prime);
			xmm3 = _mm_mullo_epi32(xmm3, fnv_prime);
			xmm0 = _mm_xor_si128(xmm0, parent->xmm[0]);
			xmm1 = _mm_xor_si128(xmm1, parent->xmm[1]);
			xmm2 = _mm_xor_si128(xmm2, parent->xmm[2]);
			xmm3 = _mm_xor_si128(xmm3, parent->xmm[3]);

			// have to write to ret as values are used to compute index
			ret->xmm[0] = xmm0;
			ret->xmm[1] = xmm1;
			ret->xmm[2] = xmm2;
			ret->xmm[3] = xmm3;
		}
		#else
		{
			for (unsigned w = 0; w != NODE_WORDS; ++w) {
				ret->words[w] = fnv_hash(ret->words[w], parent->words[w]);
			}
		}
#endif
	}
	SHA3_512(ret->bytes, ret->bytes, sizeof(node));
}

bool etchash_compute_full_data(
	void* mem,
	uint64_t full_size,
	etchash_light_t const light,
	etchash_callback_t callback
)
{
	if (full_size % (sizeof(uint32_t) * MIX_WORDS) != 0 ||
		(full_size % sizeof(node)) != 0) {
		return false;
	}
	uint32_t const max_n = (uint32_t)(full_size / sizeof(node));
	node* full_nodes = mem;
	double const progress_change = 1.0f / max_n;
	double progress = 0.0f;
	// now compute full nodes
	for (uint32_t n = 0; n != max_n; ++n) {
		if (callback &&
			n % (max_n / 100) == 0 &&
			callback((unsigned int)(ceil(progress * 100.0f))) != 0) {

			return false;
		}
		progress += progress_change;
		etchash_calculate_dag_item(&(full_nodes[n]), n, light);
	}
	return true;
}

static bool etchash_hash(
	etchash_return_value_t* ret,
	node const* full_nodes,
	etchash_light_t const light,
	uint64_t full_size,
	etchash_h256_t const header_hash,
	uint64_t const nonce
)
{
	if (full_size % MIX_WORDS != 0) {
		return false;
	}

	// pack hash and nonce together into first 40 bytes of s_mix
	assert(sizeof(node) * 8 == 512);
	node s_mix[MIX_NODES + 1];
	memcpy(s_mix[0].bytes, &header_hash, 32);
	fix_endian64(s_mix[0].double_words[4], nonce);

	// compute sha3-512 hash and replicate across mix
	SHA3_512(s_mix->bytes, s_mix->bytes, 40);
	fix_endian_arr32(s_mix[0].words, 16);

	node* const mix = s_mix + 1;
	for (uint32_t w = 0; w != MIX_WORDS; ++w) {
		mix->words[w] = s_mix[0].words[w % NODE_WORDS];
	}

	unsigned const page_size = sizeof(uint32_t) * MIX_WORDS;
	unsigned const num_full_pages = (unsigned) (full_size / page_size);

	for (unsigned i = 0; i != ETCHASH_ACCESSES; ++i) {
		uint32_t const index = fnv_hash(s_mix->words[0] ^ i, mix->words[i % MIX_WORDS]) % num_full_pages;

		for (unsigned n = 0; n != MIX_NODES; ++n) {
			node const* dag_node;
			if (full_nodes) {
				dag_node = &full_nodes[MIX_NODES * index + n];
			} else {
				node tmp_node;
				etchash_calculate_dag_item(&tmp_node, index * MIX_NODES + n, light);
				dag_node = &tmp_node;
			}

#if defined(_M_X64) && ENABLE_SSE
			{
				__m128i fnv_prime = _mm_set1_epi32(FNV_PRIME);
				__m128i xmm0 = _mm_mullo_epi32(fnv_prime, mix[n].xmm[0]);
				__m128i xmm1 = _mm_mullo_epi32(fnv_prime, mix[n].xmm[1]);
				__m128i xmm2 = _mm_mullo_epi32(fnv_prime, mix[n].xmm[2]);
				__m128i xmm3 = _mm_mullo_epi32(fnv_prime, mix[n].xmm[3]);
				mix[n].xmm[0] = _mm_xor_si128(xmm0, dag_node->xmm[0]);
				mix[n].xmm[1] = _mm_xor_si128(xmm1, dag_node->xmm[1]);
				mix[n].xmm[2] = _mm_xor_si128(xmm2, dag_node->xmm[2]);
				mix[n].xmm[3] = _mm_xor_si128(xmm3, dag_node->xmm[3]);
			}
			#else
			{
				for (unsigned w = 0; w != NODE_WORDS; ++w) {
					mix[n].words[w] = fnv_hash(mix[n].words[w], dag_node->words[w]);
				}
			}
#endif
		}

	}

	// compress mix
	for (uint32_t w = 0; w != MIX_WORDS; w += 4) {
		uint32_t reduction = mix->words[w + 0];
		reduction = reduction * FNV_PRIME ^ mix->words[w + 1];
		reduction = reduction * FNV_PRIME ^ mix->words[w + 2];
		reduction = reduction * FNV_PRIME ^ mix->words[w + 3];
		mix->words[w / 4] = reduction;
	}

	fix_endian_arr32(mix->words, MIX_WORDS / 4);
	memcpy(&ret->mix_hash, mix->bytes, 32);
	// final Keccak hash
	SHA3_256(&ret->result, s_mix->bytes, 64 + 32); // Keccak-256(s + compressed_mix)
	return true;
}

void etchash_quick_hash(
	etchash_h256_t* return_hash,
	etchash_h256_t const* header_hash,
	uint64_t nonce,
	etchash_h256_t const* mix_hash
)
{
	uint8_t buf[64 + 32];
	memcpy(buf, header_hash, 32);
	fix_endian64_same(nonce);
	memcpy(&(buf[32]), &nonce, 8);
	SHA3_512(buf, buf, 40);
	memcpy(&(buf[64]), mix_hash, 32);
	SHA3_256(return_hash, buf, 64 + 32);
}

etchash_h256_t etchash_get_seedhash(uint64_t block_number)
{
	etchash_h256_t ret;
	etchash_h256_reset(&ret);

	// Applied the specification from https://github.com/ethereumclassic/ECIPs/blob/master/_specs/ecip-1099.md#specification
	uint64_t epoch = get_epoch_number(block_number);
	if (block_number >= ETCHASH_ACTIVATION_BLOCK) {
		epoch = epoch * ETCHASH_NEW_EPOCH_LENGTH + 1;
	} else {
		epoch = epoch * ETCHASH_EPOCH_LENGTH + 1;
	}
	uint64_t const epochs = epoch / ETCHASH_EPOCH_LENGTH;
	for (uint32_t i = 0; i < epochs; ++i)
		SHA3_256(&ret, (uint8_t*)&ret, 32);
	return ret;
}

bool etchash_quick_check_difficulty(
	etchash_h256_t const* header_hash,
	uint64_t const nonce,
	etchash_h256_t const* mix_hash,
	etchash_h256_t const* boundary
)
{

	etchash_h256_t return_hash;
	etchash_quick_hash(&return_hash, header_hash, nonce, mix_hash);
	return etchash_check_difficulty(&return_hash, boundary);
}

etchash_light_t etchash_light_new_internal(uint64_t cache_size, etchash_h256_t const* seed)
{
	struct etchash_light *ret;
	ret = calloc(sizeof(*ret), 1);
	if (!ret) {
		return NULL;
	}
	ret->cache = malloc((size_t)cache_size);
	if (!ret->cache) {
		goto fail_free_light;
	}
	node* nodes = (node*)ret->cache;
	if (!etchash_compute_cache_nodes(nodes, cache_size, seed)) {
		goto fail_free_cache_mem;
	}
	ret->cache_size = cache_size;
	return ret;

fail_free_cache_mem:
	free(ret->cache);
fail_free_light:
	free(ret);
	return NULL;
}

etchash_light_t etchash_light_new(uint64_t block_number)
{
	etchash_h256_t seedhash = etchash_get_seedhash(block_number);
	etchash_light_t ret;
	ret = etchash_light_new_internal(etchash_get_cachesize(block_number), &seedhash);
	ret->block_number = block_number;
	return ret;
}

void etchash_light_delete(etchash_light_t light)
{
	if (light->cache) {
		free(light->cache);
	}
	free(light);
}

etchash_return_value_t etchash_light_compute_internal(
	etchash_light_t light,
	uint64_t full_size,
	etchash_h256_t const header_hash,
	uint64_t nonce
)
{
  	etchash_return_value_t ret;
	ret.success = true;
	if (!etchash_hash(&ret, NULL, light, full_size, header_hash, nonce)) {
		ret.success = false;
	}
	return ret;
}

etchash_return_value_t etchash_light_compute(
	etchash_light_t light,
	etchash_h256_t const header_hash,
	uint64_t nonce
)
{
	uint64_t full_size = etchash_get_datasize(light->block_number);
	return etchash_light_compute_internal(light, full_size, header_hash, nonce);
}

static bool etchash_mmap(struct etchash_full* ret, FILE* f)
{
	int fd;
	char* mmapped_data;
	errno = 0;
	ret->file = f;
	if ((fd = etchash_fileno(ret->file)) == -1) {
		return false;
	}
	mmapped_data= mmap(
		NULL,
		(size_t)ret->file_size + ETCHASH_DAG_MAGIC_NUM_SIZE,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		fd,
		0
	);
	if (mmapped_data == MAP_FAILED) {
		return false;
	}
	ret->data = (node*)(mmapped_data + ETCHASH_DAG_MAGIC_NUM_SIZE);
	return true;
}

etchash_full_t etchash_full_new_internal(
	char const* dirname,
	etchash_h256_t const seed_hash,
	uint64_t full_size,
	etchash_light_t const light,
	etchash_callback_t callback
)
{
	struct etchash_full* ret;
	FILE *f = NULL;
	ret = calloc(sizeof(*ret), 1);
	if (!ret) {
		return NULL;
	}
	ret->file_size = (size_t)full_size;
	switch (etchash_io_prepare(dirname, seed_hash, &f, (size_t)full_size, false)) {
	case ETCHASH_IO_FAIL:
		// etchash_io_prepare will do all ETCHASH_CRITICAL() logging in fail case
		goto fail_free_full;
	case ETCHASH_IO_MEMO_MATCH:
		if (!etchash_mmap(ret, f)) {
			ETCHASH_CRITICAL("mmap failure()");
			goto fail_close_file;
		}
		return ret;
	case ETCHASH_IO_MEMO_SIZE_MISMATCH:
		// if a DAG of same filename but unexpected size is found, silently force new file creation
		if (etchash_io_prepare(dirname, seed_hash, &f, (size_t)full_size, true) != ETCHASH_IO_MEMO_MISMATCH) {
			ETCHASH_CRITICAL("Could not recreate DAG file after finding existing DAG with unexpected size.");
			goto fail_free_full;
		}
		// fallthrough to the mismatch case here, DO NOT go through match
	case ETCHASH_IO_MEMO_MISMATCH:
		if (!etchash_mmap(ret, f)) {
			ETCHASH_CRITICAL("mmap failure()");
			goto fail_close_file;
		}
		break;
	}

	if (!etchash_compute_full_data(ret->data, full_size, light, callback)) {
		ETCHASH_CRITICAL("Failure at computing DAG data.");
		goto fail_free_full_data;
	}

	// after the DAG has been filled then we finalize it by writting the magic number at the beginning
	if (fseek(f, 0, SEEK_SET) != 0) {
		ETCHASH_CRITICAL("Could not seek to DAG file start to write magic number.");
		goto fail_free_full_data;
	}
	uint64_t const magic_num = ETCHASH_DAG_MAGIC_NUM;
	if (fwrite(&magic_num, ETCHASH_DAG_MAGIC_NUM_SIZE, 1, f) != 1) {
		ETCHASH_CRITICAL("Could not write magic number to DAG's beginning.");
		goto fail_free_full_data;
	}
	if (fflush(f) != 0) {// make sure the magic number IS there
		ETCHASH_CRITICAL("Could not flush memory mapped data to DAG file. Insufficient space?");
		goto fail_free_full_data;
	}
	return ret;

fail_free_full_data:
	// could check that munmap(..) == 0 but even if it did not can't really do anything here
	munmap(ret->data, (size_t)full_size);
fail_close_file:
	fclose(ret->file);
fail_free_full:
	free(ret);
	return NULL;
}

etchash_full_t etchash_full_new(etchash_light_t light, etchash_callback_t callback)
{
	char strbuf[256];
	if (!etchash_get_default_dirname(strbuf, 256)) {
		return NULL;
	}
	uint64_t full_size = etchash_get_datasize(light->block_number);
	etchash_h256_t seedhash = etchash_get_seedhash(light->block_number);
	return etchash_full_new_internal(strbuf, seedhash, full_size, light, callback);
}

void etchash_full_delete(etchash_full_t full)
{
	// could check that munmap(..) == 0 but even if it did not can't really do anything here
	munmap(full->data, (size_t)full->file_size);
	if (full->file) {
		fclose(full->file);
	}
	free(full);
}

etchash_return_value_t etchash_full_compute(
	etchash_full_t full,
	etchash_h256_t const header_hash,
	uint64_t nonce
)
{
	etchash_return_value_t ret;
	ret.success = true;
	if (!etchash_hash(
		&ret,
		(node const*)full->data,
		NULL,
		full->file_size,
		header_hash,
		nonce)) {
		ret.success = false;
	}
	return ret;
}

void const* etchash_full_dag(etchash_full_t full)
{
	return full->data;
}

uint64_t etchash_full_dag_size(etchash_full_t full)
{
	return full->file_size;
}

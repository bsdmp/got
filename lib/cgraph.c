/*
 * Copyright (c) 2026 YOUR NAME HERE <YOUR EMAIL HERE>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sha1.h>
#include <sha2.h>
#include <endian.h>
#include <unistd.h>
#include <time.h>

#include "got_error.h"
#include "got_object.h"

#include "got_lib_hash.h"
#include "got_lib_cgraph.h"

/* On-disk header, chunk-lookup, and "commit data" layout constants. */
#define GOT_CGRAPH_SIGNATURE0		'C'
#define GOT_CGRAPH_SIGNATURE1		'G'
#define GOT_CGRAPH_SIGNATURE2		'P'
#define GOT_CGRAPH_SIGNATURE3		'H'

#define GOT_CGRAPH_VERSION		1

#define GOT_CGRAPH_HASH_VERSION_SHA1	1
#define GOT_CGRAPH_HASH_VERSION_SHA256	2

#define GOT_CGRAPH_HEADER_LEN		8
#define GOT_CGRAPH_CHUNKTBL_ENTRY_LEN	12

#define GOT_CGRAPH_CHUNKID_OIDF	0x4f494446 /* "OIDF" */
#define GOT_CGRAPH_CHUNKID_OIDL	0x4f49444c /* "OIDL" */
#define GOT_CGRAPH_CHUNKID_CDAT	0x43444154 /* "CDAT" */
#define GOT_CGRAPH_CHUNKID_EDGE	0x45444745 /* "EDGE" */

#define GOT_CGRAPH_FANOUT_NITEMS	256

/* Special values used in the Commit Data (CDAT) parent-position fields. */
#define GOT_CGRAPH_PARENT_NONE		0x70000000
#define GOT_CGRAPH_EXTRA_EDGE_NEEDED	0x80000000
#define GOT_CGRAPH_LAST_EDGE		0x80000000
#define GOT_CGRAPH_EDGE_POS_MASK	0x7fffffff

struct got_cgraph {
	enum got_hash_algorithm algo;
	size_t digest_len;

	uint32_t ncommits;

	uint32_t fanout[GOT_CGRAPH_FANOUT_NITEMS];
	uint8_t *oidlookup;	/* ncommits * digest_len bytes, sorted */
	uint8_t *commitdata;	/* ncommits * (digest_len + 16) bytes */

	uint32_t *edgelist;	/* nedges entries, host byte order */
	uint32_t nedges;
};

struct got_cgraph_chunkent {
	uint32_t id;
	uint64_t offset;
};

static int
chunkent_cmp(const void *pa, const void *pb)
{
	const struct got_cgraph_chunkent *a = pa, *b = pb;

	if (a->offset < b->offset)
		return -1;
	if (a->offset > b->offset)
		return 1;
	return 0;
}

static const struct got_error *
read_n(int fd, void *buf, size_t len)
{
	ssize_t n;
	size_t off = 0;

	while (off < len) {
		n = read(fd, (char *)buf + off, len - off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return got_error_from_errno("read");
		}
		if (n == 0)
			return got_error(GOT_ERR_BAD_CGRAPH);
		off += n;
	}
	return NULL;
}

static const struct got_error *
pread_n(int fd, void *buf, size_t len, off_t off)
{
	ssize_t n;
	size_t total = 0;

	while (total < len) {
		n = pread(fd, (char *)buf + total, len - total, off + total);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return got_error_from_errno("pread");
		}
		if (n == 0)
			return got_error(GOT_ERR_BAD_CGRAPH);
		total += n;
	}
	return NULL;
}

/*
 * Find the chunk with the given ID in a chunk lookup table which has
 * already been sorted by file offset. 'nchunks' is the number of real
 * chunks, not counting the trailing terminator entry. On success, the
 * chunk's offset and length (inferred from the next chunk in file
 * order, or from the terminator) are returned.
 */
static int
find_chunk(uint64_t *out_offset, uint64_t *out_len,
    struct got_cgraph_chunkent *sorted, int nchunks, uint32_t id)
{
	int i;

	for (i = 0; i < nchunks; i++) {
		if (sorted[i].id == id) {
			*out_offset = sorted[i].offset;
			*out_len = sorted[i + 1].offset - sorted[i].offset;
			return 1;
		}
	}
	return 0;
}

const struct got_error *
got_cgraph_open(struct got_cgraph **cgraph, int dir_fd,
    enum got_hash_algorithm algo)
{
	const struct got_error *err = NULL;
	struct got_cgraph *cg = NULL;
	int fd = -1;
	struct stat sb;
	uint8_t hdr[GOT_CGRAPH_HEADER_LEN];
	int version, hash_version, nchunks, nbase;
	struct got_cgraph_chunkent *chunks = NULL;
	uint64_t oidf_off, oidf_len, oidl_off, oidl_len;
	uint64_t cdat_off, cdat_len, edge_off, edge_len;
	uint64_t body_end;
	size_t digest_len;
	int i, have_edge;

	*cgraph = NULL;

	fd = openat(dir_fd, GOT_CGRAPH_PATH,
	    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1) {
		if (errno == ENOENT) {
			/*
			 * Treat a missing file the same as an unusable one,
			 * so that got_cgraph_try_open() can tell the two
			 * "no commit-graph available" cases apart from a
			 * genuine I/O error by checking err->code alone,
			 * without having to rely on the value of errno
			 * after this function has already returned.
			 */
			err = got_error(GOT_ERR_BAD_CGRAPH);
		} else
			err = got_error_from_errno2("openat", GOT_CGRAPH_PATH);
		goto done;
	}

	if (fstat(fd, &sb) == -1) {
		err = got_error_from_errno2("fstat", GOT_CGRAPH_PATH);
		goto done;
	}

	if (sb.st_size < GOT_CGRAPH_HEADER_LEN + GOT_CGRAPH_CHUNKTBL_ENTRY_LEN) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	err = read_n(fd, hdr, sizeof(hdr));
	if (err)
		goto done;

	if (hdr[0] != GOT_CGRAPH_SIGNATURE0 || hdr[1] != GOT_CGRAPH_SIGNATURE1 ||
	    hdr[2] != GOT_CGRAPH_SIGNATURE2 || hdr[3] != GOT_CGRAPH_SIGNATURE3) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	version = hdr[4];
	hash_version = hdr[5];
	nchunks = hdr[6];
	nbase = hdr[7];

	if (version != GOT_CGRAPH_VERSION) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	/*
	 * Split commit-graph chains are not supported: refuse any file
	 * which claims to build on top of "nbase" earlier graph files.
	 */
	if (nbase != 0) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	if (hash_version == GOT_CGRAPH_HASH_VERSION_SHA1)
		digest_len = got_hash_digest_length(GOT_HASH_SHA1);
	else if (hash_version == GOT_CGRAPH_HASH_VERSION_SHA256)
		digest_len = got_hash_digest_length(GOT_HASH_SHA256);
	else {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	if ((hash_version == GOT_CGRAPH_HASH_VERSION_SHA1 &&
	    algo != GOT_HASH_SHA1) ||
	    (hash_version == GOT_CGRAPH_HASH_VERSION_SHA256 &&
	    algo != GOT_HASH_SHA256)) {
		/* Hash algorithm mismatch: file is unusable for this repo. */
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	if (nchunks < 1) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	/* +1 for the terminating entry. */
	if ((uint64_t)sb.st_size < GOT_CGRAPH_HEADER_LEN +
	    (uint64_t)(nchunks + 1) * GOT_CGRAPH_CHUNKTBL_ENTRY_LEN) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	chunks = calloc(nchunks + 1, sizeof(*chunks));
	if (chunks == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	for (i = 0; i < nchunks + 1; i++) {
		uint8_t buf[GOT_CGRAPH_CHUNKTBL_ENTRY_LEN];
		uint32_t id;
		uint64_t off;

		err = read_n(fd, buf, sizeof(buf));
		if (err)
			goto done;
		memcpy(&id, buf, sizeof(id));
		memcpy(&off, buf + 4, sizeof(off));
		chunks[i].id = be32toh(id);
		chunks[i].offset = be64toh(off);
	}
	if (chunks[nchunks].id != 0) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	qsort(chunks, nchunks + 1, sizeof(*chunks), chunkent_cmp);
	body_end = chunks[nchunks].offset;
	if ((uint64_t)sb.st_size < body_end + digest_len) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	if (!find_chunk(&oidf_off, &oidf_len, chunks, nchunks,
	    GOT_CGRAPH_CHUNKID_OIDF) ||
	    oidf_len != GOT_CGRAPH_FANOUT_NITEMS * sizeof(uint32_t)) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	cg = calloc(1, sizeof(*cg));
	if (cg == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}
	cg->algo = algo;
	cg->digest_len = digest_len;

	{
		uint32_t raw[GOT_CGRAPH_FANOUT_NITEMS];

		err = pread_n(fd, raw, sizeof(raw), oidf_off);
		if (err)
			goto done;
		for (i = 0; i < GOT_CGRAPH_FANOUT_NITEMS; i++)
			cg->fanout[i] = be32toh(raw[i]);
	}
	cg->ncommits = cg->fanout[GOT_CGRAPH_FANOUT_NITEMS - 1];

	if (!find_chunk(&oidl_off, &oidl_len, chunks, nchunks,
	    GOT_CGRAPH_CHUNKID_OIDL) ||
	    oidl_len != (uint64_t)cg->ncommits * digest_len) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}
	if (!find_chunk(&cdat_off, &cdat_len, chunks, nchunks,
	    GOT_CGRAPH_CHUNKID_CDAT) ||
	    cdat_len != (uint64_t)cg->ncommits * (digest_len + 16)) {
		err = got_error(GOT_ERR_BAD_CGRAPH);
		goto done;
	}

	if (cg->ncommits > 0) {
		cg->oidlookup = malloc(oidl_len);
		if (cg->oidlookup == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		err = pread_n(fd, cg->oidlookup, oidl_len, oidl_off);
		if (err)
			goto done;

		cg->commitdata = malloc(cdat_len);
		if (cg->commitdata == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		err = pread_n(fd, cg->commitdata, cdat_len, cdat_off);
		if (err)
			goto done;
	}

	have_edge = find_chunk(&edge_off, &edge_len, chunks, nchunks,
	    GOT_CGRAPH_CHUNKID_EDGE);
	if (have_edge) {
		uint32_t *raw = NULL;

		if (edge_len % sizeof(uint32_t) != 0) {
			err = got_error(GOT_ERR_BAD_CGRAPH);
			goto done;
		}
		cg->nedges = edge_len / sizeof(uint32_t);
		if (cg->nedges > 0) {
			raw = malloc(edge_len);
			if (raw == NULL) {
				err = got_error_from_errno("malloc");
				goto done;
			}
			err = pread_n(fd, raw, edge_len, edge_off);
			if (err) {
				free(raw);
				goto done;
			}
			cg->edgelist = malloc(edge_len);
			if (cg->edgelist == NULL) {
				err = got_error_from_errno("malloc");
				free(raw);
				goto done;
			}
			for (i = 0; i < (int)cg->nedges; i++)
				cg->edgelist[i] = be32toh(raw[i]);
			free(raw);
		}
	}

	*cgraph = cg;
	cg = NULL;
done:
	free(chunks);
	if (cg)
		got_cgraph_close(cg);
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	return err;
}

const struct got_error *
got_cgraph_try_open(struct got_cgraph **cgraph, int dir_fd,
    enum got_hash_algorithm algo)
{
	const struct got_error *err;

	*cgraph = NULL;

	err = got_cgraph_open(cgraph, dir_fd, algo);
	if (err == NULL)
		return NULL;

	if (err->code == GOT_ERR_BAD_CGRAPH) {
		*cgraph = NULL;
		return NULL;
	}

	return err;
}

void
got_cgraph_close(struct got_cgraph *cgraph)
{
	if (cgraph == NULL)
		return;

	free(cgraph->oidlookup);
	free(cgraph->commitdata);
	free(cgraph->edgelist);
	free(cgraph);
}

int
got_cgraph_find(struct got_cgraph *cgraph, struct got_object_id *id)
{
	uint8_t id0 = id->hash[0];
	int left = 0, right = (int)cgraph->ncommits - 1;

	if (id0 > 0)
		left = cgraph->fanout[id0 - 1];

	while (left <= right) {
		uint8_t *oid;
		int i, cmp;

		i = (left + right) / 2;
		oid = cgraph->oidlookup + (size_t)i * cgraph->digest_len;
		cmp = memcmp(id->hash, oid, cgraph->digest_len);
		if (cmp == 0)
			return i;
		else if (cmp > 0)
			left = i + 1;
		else
			right = i - 1;
	}

	return -1;
}

static uint8_t *
commit_row(struct got_cgraph *cgraph, int idx)
{
	return cgraph->commitdata + (size_t)idx * (cgraph->digest_len + 16);
}

uint32_t
got_cgraph_get_generation(struct got_cgraph *cgraph, int idx)
{
	uint8_t *row = commit_row(cgraph, idx);
	uint32_t word1;

	memcpy(&word1, row + cgraph->digest_len + 8, sizeof(word1));
	word1 = be32toh(word1);

	return word1 >> 2;
}

time_t
got_cgraph_get_committer_time(struct got_cgraph *cgraph, int idx)
{
	uint8_t *row = commit_row(cgraph, idx);
	uint32_t word1, word2;

	memcpy(&word1, row + cgraph->digest_len + 8, sizeof(word1));
	memcpy(&word2, row + cgraph->digest_len + 12, sizeof(word2));
	word1 = be32toh(word1);
	word2 = be32toh(word2);

	return (time_t)(((uint64_t)(word1 & 0x3) << 32) | word2);
}

void
got_cgraph_get_tree_id(struct got_cgraph *cgraph, int idx,
    struct got_object_id *id)
{
	uint8_t *row = commit_row(cgraph, idx);

	memset(id, 0, sizeof(*id));
	id->algo = cgraph->algo;
	memcpy(id->hash, row, cgraph->digest_len);
}

static void
resolve_id(struct got_cgraph *cgraph, uint32_t pos, struct got_object_id *id)
{
	memset(id, 0, sizeof(*id));
	id->algo = cgraph->algo;
	memcpy(id->hash, cgraph->oidlookup + (size_t)pos * cgraph->digest_len,
	    cgraph->digest_len);
}

static const struct got_error *
queue_parent(struct got_object_id_queue *ids, struct got_object_id *id)
{
	const struct got_error *err;
	struct got_object_qid *qid;

	err = got_object_qid_alloc(&qid, id);
	if (err)
		return err;

	STAILQ_INSERT_TAIL(ids, qid, entry);
	return NULL;
}

const struct got_error *
got_cgraph_get_parent_ids(struct got_cgraph *cgraph, int idx,
    struct got_object_id_queue *ids)
{
	const struct got_error *err = NULL;
	uint8_t *row = commit_row(cgraph, idx);
	uint32_t parent1, parent2;
	struct got_object_id id;
	struct got_object_id_queue tmp;

	STAILQ_INIT(&tmp);

	memcpy(&parent1, row + cgraph->digest_len, sizeof(parent1));
	memcpy(&parent2, row + cgraph->digest_len + 4, sizeof(parent2));
	parent1 = be32toh(parent1);
	parent2 = be32toh(parent2);

	if (parent1 != GOT_CGRAPH_PARENT_NONE) {
		if (parent1 >= cgraph->ncommits) {
			err = got_error(GOT_ERR_BAD_CGRAPH);
			goto done;
		}
		resolve_id(cgraph, parent1, &id);
		err = queue_parent(&tmp, &id);
		if (err)
			goto done;
	}

	if (parent2 == GOT_CGRAPH_PARENT_NONE) {
		/* No second parent. */
	} else if (parent2 & GOT_CGRAPH_EXTRA_EDGE_NEEDED) {
		uint32_t epos = parent2 & GOT_CGRAPH_EDGE_POS_MASK;

		for (;;) {
			uint32_t eval, pos;
			int last;

			if (cgraph->edgelist == NULL ||
			    epos >= cgraph->nedges) {
				err = got_error(GOT_ERR_BAD_CGRAPH);
				goto done;
			}
			eval = cgraph->edgelist[epos];
			last = (eval & GOT_CGRAPH_LAST_EDGE) != 0;
			pos = eval & GOT_CGRAPH_EDGE_POS_MASK;
			if (pos >= cgraph->ncommits) {
				err = got_error(GOT_ERR_BAD_CGRAPH);
				goto done;
			}
			resolve_id(cgraph, pos, &id);
			err = queue_parent(&tmp, &id);
			if (err)
				goto done;
			if (last)
				break;
			epos++;
		}
	} else {
		if (parent2 >= cgraph->ncommits) {
			err = got_error(GOT_ERR_BAD_CGRAPH);
			goto done;
		}
		resolve_id(cgraph, parent2, &id);
		err = queue_parent(&tmp, &id);
		if (err)
			goto done;
	}

	STAILQ_CONCAT(ids, &tmp);
done:
	if (err)
		got_object_id_queue_free(&tmp);
	return err;
}

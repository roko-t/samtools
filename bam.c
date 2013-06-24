#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include "bam.h"
#include "htslib/kstring.h"
#include "sam_header.h"

char *bam_format1(const bam_header_t *header, const bam1_t *b)
{
	kstring_t str;
	str.l = str.m = 0; str.s = NULL;
	sam_format1(header, b, &str);
	return str.s;
}

void bam_view1(const bam_header_t *header, const bam1_t *b)
{
	char *s = bam_format1(header, b);
	puts(s);
	free(s);
}

int bam_validate1(const bam_header_t *header, const bam1_t *b)
{
	char *s;

	if (b->core.tid < -1 || b->core.mtid < -1) return 0;
	if (header && (b->core.tid >= header->n_targets || b->core.mtid >= header->n_targets)) return 0;

	if (b->data_len < b->core.l_qname) return 0;
	s = memchr(bam1_qname(b), '\0', b->core.l_qname);
	if (s != &bam1_qname(b)[b->core.l_qname-1]) return 0;

	// FIXME: Other fields could also be checked, especially the auxiliary data

	return 1;
}

// FIXME: we should also check the LB tag associated with each alignment
const char *bam_get_library(bam_header_t *h, const bam1_t *b)
{
#if 0
	const uint8_t *rg;
	if (h->dict == 0) h->dict = sam_header_parse2(h->text);
	if (h->rg2lib == 0) h->rg2lib = sam_header2tbl(h->dict, "RG", "ID", "LB");
	rg = bam_aux_get(b, "RG");
	return (rg == 0)? 0 : sam_tbl_get(h->rg2lib, (const char*)(rg + 1));
#else
	fprintf(stderr, "Samtools-htslib-API: bam_get_library() not yet implemented\n");
	abort();
#endif
}

/************
 * Remove B *
 ************/

#define bam1_seq_seti(s, i, c) ( (s)[(i)>>1] = ((s)[(i)>>1] & 0xf<<(((i)&1)<<2)) | (c)<<((~(i)&1)<<2) )

int bam_remove_B(bam1_t *b)
{
	int i, j, end_j, k, l, no_qual;
	uint32_t *cigar, *new_cigar;
	uint8_t *seq, *qual, *p;
	// test if removal is necessary
	if (b->core.flag & BAM_FUNMAP) return 0; // unmapped; do nothing
	cigar = bam1_cigar(b);
	for (k = 0; k < b->core.n_cigar; ++k)
		if (bam_cigar_op(cigar[k]) == BAM_CBACK) break;
	if (k == b->core.n_cigar) return 0; // no 'B'
	if (bam_cigar_op(cigar[0]) == BAM_CBACK) goto rmB_err; // cannot be removed
	// allocate memory for the new CIGAR
	if (b->data_len + (b->core.n_cigar + 1) * 4 > b->m_data) { // not enough memory
		b->m_data = b->data_len + b->core.n_cigar * 4;
		kroundup32(b->m_data);
		b->data = (uint8_t*)realloc(b->data, b->m_data);
		cigar = bam1_cigar(b); // after realloc, cigar may be changed
	}
	new_cigar = (uint32_t*)(b->data + (b->m_data - b->core.n_cigar * 4)); // from the end of b->data
	// the core loop
	seq = bam1_seq(b); qual = bam1_qual(b);
	no_qual = (qual[0] == 0xff); // test whether base quality is available
	i = j = 0; end_j = -1;
	for (k = l = 0; k < b->core.n_cigar; ++k) {
		int op  = bam_cigar_op(cigar[k]);
		int len = bam_cigar_oplen(cigar[k]);
		if (op == BAM_CBACK) { // the backward operation
			int t, u;
			if (k == b->core.n_cigar - 1) break; // ignore 'B' at the end of CIGAR
			if (len > j) goto rmB_err; // an excessively long backward
			for (t = l - 1, u = 0; t >= 0; --t) { // look back
				int op1  = bam_cigar_op(new_cigar[t]);
				int len1 = bam_cigar_oplen(new_cigar[t]);
				if (bam_cigar_type(op1)&1) { // consume the query
					if (u + len1 >= len) { // stop
						new_cigar[t] -= (len - u) << BAM_CIGAR_SHIFT;
						break;
					} else u += len1;
				}
			}
			if (bam_cigar_oplen(new_cigar[t]) == 0) --t; // squeeze out the zero-length operation
			l = t + 1;
			end_j = j; j -= len;
		} else { // other CIGAR operations
			new_cigar[l++] = cigar[k];
			if (bam_cigar_type(op)&1) { // consume the query
				if (i != j) { // no need to copy if i == j
					int u, c, c0;
					for (u = 0; u < len; ++u) { // construct the consensus
						c = bam1_seqi(seq, i+u);
						if (j + u < end_j) { // in an overlap
							c0 = bam1_seqi(seq, j+u);
							if (c != c0) { // a mismatch; choose the better base
								if (qual[j+u] < qual[i+u]) { // the base in the 2nd segment is better
									bam1_seq_seti(seq, j+u, c);
									qual[j+u] = qual[i+u] - qual[j+u];
								} else qual[j+u] -= qual[i+u]; // the 1st is better; reduce base quality
							} else qual[j+u] = qual[j+u] > qual[i+u]? qual[j+u] : qual[i+u];
						} else { // not in an overlap; copy over
							bam1_seq_seti(seq, j+u, c);
							qual[j+u] = qual[i+u];
						}
					}
				}
				i += len, j += len;
			}
		}
	}
	if (no_qual) qual[0] = 0xff; // in very rare cases, this may be modified
	// merge adjacent operations if possible
	for (k = 1; k < l; ++k)
		if (bam_cigar_op(new_cigar[k]) == bam_cigar_op(new_cigar[k-1]))
			new_cigar[k] += new_cigar[k-1] >> BAM_CIGAR_SHIFT << BAM_CIGAR_SHIFT, new_cigar[k-1] &= 0xf;
	// kill zero length operations
	for (k = i = 0; k < l; ++k)
		if (new_cigar[k] >> BAM_CIGAR_SHIFT)
			new_cigar[i++] = new_cigar[k];
	l = i;
	// update b
	memcpy(cigar, new_cigar, l * 4); // set CIGAR
	p = b->data + b->core.l_qname + l * 4;
	memmove(p, seq, (j+1)>>1); p += (j+1)>>1; // set SEQ
	memmove(p, qual, j); p += j; // set QUAL
	memmove(p, bam1_aux(b), bam_get_l_aux(b)); p += bam_get_l_aux(b); // set optional fields
	b->core.n_cigar = l, b->core.l_qseq = j; // update CIGAR length and query length
	b->data_len = p - b->data; // update record length
	return 0;

rmB_err:
	b->core.flag |= BAM_FUNMAP;
	return -1;
}

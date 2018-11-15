/****************************************************************************
 *       Experimenting with alternate rhdf5::h5read() implementations       *
 *                            Author: H. Pag\`es                            *
 ****************************************************************************/
#include "HDF5Array.h"
#include "hdf5.h"

#include <stdlib.h>  /* for malloc, free */
#include <string.h>  /* for memcpy */
#include <limits.h>  /* for INT_MAX, LLONG_MAX, LLONG_MIN */


static char errmsg_buf[256];

/* 'counts' can be R_NilValue. */
static int shallow_check_starts_counts(SEXP starts, SEXP counts)
{
	int starts_len, counts_len;

	if (!isVectorList(starts)) {  // IS_LIST() is broken
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "'starts' must be a list");
		return -1;
	}
	starts_len = LENGTH(starts);
	if (counts != R_NilValue) {
		if (!isVectorList(counts)) {  // IS_LIST() is broken
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "'counts' must be a list (or NULL)");
			return -1;
		}
		counts_len = LENGTH(counts);
		if (starts_len != counts_len) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "'starts' and 'counts' must have "
				 "the same length");
			return -1;
		}
	}
	return starts_len;
}

#define	NOT_A_FINITE_NUMBER(x) \
	(R_IsNA(x) || R_IsNaN(x) || (x) == R_PosInf || (x) == R_NegInf)

static int get_elt_as_llint(SEXP x, int i, long long int *val,
			    const char *what, int along)
{
	int tmp1;
	double tmp2;

	if (IS_INTEGER(x)) {
		tmp1 = INTEGER(x)[i];
		if (tmp1 == NA_INTEGER) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "%s[[%d]][%d] is NA",
				 what, along + 1, i + 1);
			return -1;
		}
		*val = (long long int) tmp1;
	} else {
		tmp2 = REAL(x)[i];
		if (NOT_A_FINITE_NUMBER(tmp2)) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "%s[[%d]][%d] is NA or NaN "
				 "or not a finite number",
				 what, along + 1, i + 1);
			return -1;
		}
		if (tmp2 > (double) LLONG_MAX ||
		    tmp2 < (double) LLONG_MIN) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "%s[[%d]][%d] is too large (= %e)",
				 what, along + 1, i + 1, tmp2);
			return -1;
		}
		*val = (long long int) tmp2;
	}
	return 0;
}

static int check_starts_counts_along(int along,
				     int dset_rank, const hsize_t *dset_dims,
				     SEXP starts, SEXP counts,
				     int *nregion, int *count_sums)
{
	SEXP start, count;
	int n, i, ret;
	long long int count_sum, c, e, s;

	start = VECTOR_ELT(starts, along);
	if (!(IS_INTEGER(start) || IS_NUMERIC(start))) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "'starts[[%d]]' must be "
			 "an integer vector", along + 1);
		return -1;
	}
	n = LENGTH(start);
	if (counts != R_NilValue) {
		count = VECTOR_ELT(counts, along);
		if (!(IS_INTEGER(count) || IS_NUMERIC(count))) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "'counts[[%d]]' must be "
				 "an integer vector", along + 1);
			return -1;
		}
		if (LENGTH(count) != n) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "'counts[[%d]]' must have the "
				 "same length as 'starts[[%d]]'",
				 along + 1, along + 1);
			return -1;
		}
	}
	nregion[along] = n;
	count_sum = 0;
	c = 1;
	e = 0;
	for (i = 0; i < n; i++) {
		ret = get_elt_as_llint(start, i, &s, "starts", along);
		if (ret < 0)
			return -1;
		if (s <= e) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "starts[[%d]][%d] is <= 0 or < "
				 "starts[[%d]][%d] + counts[[%d]][%d]",
				 along + 1, i + 1,
				 along + 1, i,
				 along + 1, i);
			return -1;
		}
		if (counts != R_NilValue) {
			ret = get_elt_as_llint(count, i, &c, "counts", along);
			if (ret < 0)
				return -1;
			if (c <= 0) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
					 "counts[[%d]][%d] is <= 0",
					 along + 1, i + 1);
				return -1;
			}
		}
		e = s + c - 1;  // could overflow! (FIXME)
		if (e > dset_dims[dset_rank - 1 - along]) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "starts[[%d]][%d] + counts[[%d]][%d] - 1 "
				 "is greater than the corresponding\n  "
				 "dimension in the dataset",
				 along + 1, i + 1, along + 1, i + 1);
			return -1;
		}
		count_sum += c;  // could overflow! (FIXME)
		if (count_sum > INT_MAX) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "sum(counts[[%d]]) is too big! "
				 "(>= 2^31)", along + 1);
			return -1;
		}
	}
	count_sums[along] = count_sum;
	return 0;
}

/* 'counts' can be R_NilValue. */
static int deep_check_starts_counts(int dset_rank, const hsize_t *dset_dims,
				    SEXP starts, SEXP counts,
				    int *nregion, int *count_sums,
				    R_xlen_t *ans_len)
{
	int along, ret;

	if (!(isVectorList(starts) && LENGTH(starts) == dset_rank)) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "'starts' must be a list with one list "
			 "element per dimension in the dataset");
		return -1;
	}
	if (counts != R_NilValue) {
		if (!(isVectorList(counts) && LENGTH(counts) == dset_rank)) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "'counts' must be a list with one list "
				 "element per dimension in the dataset");
			return -1;
		}
	}
	*ans_len = 1;
	for (along = 0; along < dset_rank; along++) {
		ret = check_starts_counts_along(along, dset_rank, dset_dims,
						starts, counts,
						nregion, count_sums);
		if (ret < 0)
			return -1;
		*ans_len *= count_sums[along];
	}
	return 0;
}

/* Should we use H5Sselect_hyperslab() or H5Sselect_elements() for this?
   Useful links:
   - Documentation of H5Sselect_hyperslab() and H5Sselect_elements():
       https://support.hdfgroup.org/HDF5/doc1.8/RM/RM_H5S.html
   - Documentation of H5Dread():
       https://support.hdfgroup.org/HDF5/doc/RM/RM_H5D.html#Dataset-Read
   - A useful example:
       https://support.hdfgroup.org/HDF5/doc/Intro/IntroExamples.html#CheckAndReadExample
*/

static int add_region_to_read(hid_t file_space_id, int dset_rank,
			      SEXP starts, SEXP counts, const int *region_idx,
			      hsize_t *offset_buf, hsize_t *count_buf)
{
	int along, i, ret;
	SEXP start, count;
	hsize_t tmp;

	for (along = 0; along < dset_rank; along++) {
		i = region_idx[along];
		start = VECTOR_ELT(starts, along);
		tmp = IS_INTEGER(start) ? (hsize_t) INTEGER(start)[i]
					: (hsize_t) REAL(start)[i];
		offset_buf[dset_rank - 1 - along] = tmp - 1;
		if (counts != R_NilValue) {
			count = VECTOR_ELT(counts, along);
			tmp = IS_INTEGER(count) ? (hsize_t) INTEGER(count)[i]
						: (hsize_t) REAL(count)[i];
			count_buf[dset_rank - 1 - along] = tmp;
		}
	}
	ret = H5Sselect_hyperslab(file_space_id, H5S_SELECT_OR,
				  offset_buf, NULL, count_buf, NULL);
	if (ret < 0) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
		         "H5Sselect_hyperslab() returned an error");
		return -1;
	}
	return 0;
}

static int next_region(int dset_rank, const int *nregion, int *region_idx)
{
	int along, i;

	for (along = 0; along < dset_rank; along++) {
		i = region_idx[along] + 1;
		if (i < nregion[along]) {
			region_idx[along] = i;
			return 1;
		}
		region_idx[along] = 0;
	}
	return 0;
}

static int set_regions_to_read(hid_t file_space_id, int dset_rank,
			       SEXP starts, SEXP counts, const int *nregion)
{
	int ret, *region_idx, along;
	hsize_t *offset_buf, *count_buf;  // hyperslab offsets and dims
	long long unsigned n;

	ret = H5Sselect_none(file_space_id);
	if (ret < 0)
		return -1;
	for (along = 0; along < dset_rank; along++)
		if (nregion[along] == 0)
			return 0;  // no region to set

	/* Allocate 'offset_buf' and 'count_buf'. */
	offset_buf = (hsize_t *) malloc(2 * dset_rank * sizeof(hsize_t));
	if (offset_buf == NULL) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "failed to allocate memory for 'offset_buf' "
			 "and 'count_buf'");
		return -1;
	}
	count_buf = offset_buf + dset_rank;
	for (along = 0; along < dset_rank; along++)
		count_buf[along] = 1;

	/* Allocate and initialize 'region_idx'. */
	region_idx = (int *) malloc(dset_rank * sizeof(int));
	if (region_idx == NULL) {
		free(offset_buf);
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "failed to allocate memory for 'region_idx'");
		return -1;
	}
	for (along = 0; along < dset_rank; along++)
		region_idx[along] = 0;

	/* Set all the regions. */
	n = 0;
	do {
		n++;
		//printf("- indices for region %llu:", n);
		//for (along = 0; along < dset_rank; along++)
		//	printf(" %d", region_idx[along]);
		//printf("\n");
		ret = add_region_to_read(file_space_id, dset_rank,
					 starts, counts, region_idx,
					 offset_buf, count_buf);
		if (ret < 0)
			break;
	} while (next_region(dset_rank, nregion, region_idx));
	printf("nb of hyperslabs = %llu\n", n);
	free(region_idx);
	free(offset_buf);
	return ret;
}

static hid_t prepare_file_space(hid_t dset_id, SEXP starts, SEXP counts,
				int *count_sums, R_xlen_t *ans_len)
{
	hid_t file_space_id;
	int dset_rank, *nregion, ret;
	hsize_t *dset_dims;

	file_space_id = H5Dget_space(dset_id);
	if (file_space_id < 0) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
		         "H5Dget_space() returned an error");
		return -1;
	}

	dset_rank = H5Sget_simple_extent_ndims(file_space_id);

	/* Allocate and set 'dset_dims'. */
	dset_dims = (hsize_t *) malloc(dset_rank * sizeof(hsize_t));
	if (dset_dims == NULL) {
		H5Sclose(file_space_id);
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "failed to allocate memory for 'dset_dims'");
		return -1;
	}
	if (H5Sget_simple_extent_dims(file_space_id, dset_dims, NULL) !=
	    dset_rank)
	{
		free(dset_dims);
		H5Sclose(file_space_id);
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "H5Sget_simple_extent_dims() returned "
			 "an unexpected value");
		return -1;
	}

	/* Allocate and set 'nregion'. */
	nregion = (int *) malloc(dset_rank * sizeof(int));
	if (nregion == NULL) {
		free(dset_dims);
		H5Sclose(file_space_id);
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "failed to allocate memory for 'nregion'");
		return -1;
	}

	/* This call will populate 'nregion' and 'count_sums', and will
	   set 'ans_len'. */
	ret = deep_check_starts_counts(dset_rank, dset_dims, starts, counts,
				       nregion, count_sums, ans_len);
	free(dset_dims);
	if (ret < 0) {
		free(nregion);
		H5Sclose(file_space_id);
		return -1;
	}

	ret = set_regions_to_read(file_space_id, dset_rank,
				  starts, counts, nregion);
	free(nregion);
	if (ret < 0) {
		H5Sclose(file_space_id);
		return -1;
	}
	return file_space_id;
}

static hid_t prepare_mem_space(int dset_rank, const int *count_sums)
{
	hsize_t *dims;
	int along, ret;
	hid_t mem_space_id;

	/* Allocate and set 'dims'. */
	dims = (hsize_t *) malloc(dset_rank * sizeof(hsize_t));
	if (dims == NULL) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "failed to allocate memory for 'dims'");
		return -1;
	}
	for (along = 0; along < dset_rank; along++)
		dims[dset_rank - 1 - along] = count_sums[along];

	mem_space_id = H5Screate_simple(dset_rank, dims, NULL);
	if (mem_space_id < 0) {
		free(dims);
		snprintf(errmsg_buf, sizeof(errmsg_buf),
		         "H5Screate_simple() returned an error");
		return -1;
	}
	ret = H5Sselect_all(mem_space_id);
	if (ret < 0) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
		         "H5Sselect_hyperslab() returned an error");
		return -1;
	}
	free(dims);
	return mem_space_id;
}

/* Return R_NilValue if error. */
static SEXP h5mread(hid_t dset_id, SEXP starts, SEXP counts)
{
	int starts_len, *count_sums, along, ret;
	hid_t file_space_id, mem_space_id;
	R_xlen_t ans_len;
	SEXP ans, ans_dim;

	starts_len = shallow_check_starts_counts(starts, counts);
	if (starts_len < 0)
		return R_NilValue;

	/* Allocate 'count_sums'. */
	count_sums = (int *) malloc(starts_len * sizeof(int));
	if (count_sums == NULL) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "failed to allocate memory for 'count_sums'");
		return R_NilValue;
	}

	/* Prepare 'file_space_id'. */
	file_space_id = prepare_file_space(dset_id, starts, counts,
					   count_sums, &ans_len);
	if (file_space_id < 0) {
		free(count_sums);
		return R_NilValue;
	}

	if (ans_len != 0) {
		/* Prepare 'mem_space_id'. */
		mem_space_id = prepare_mem_space(starts_len, count_sums);
		if (mem_space_id < 0) {
			H5Sclose(file_space_id);
			free(count_sums);
			return R_NilValue;
		}
	}

	ans = PROTECT(NEW_INTEGER(ans_len));
	ans_dim = PROTECT(NEW_INTEGER(starts_len));
	for (along = 0; along < starts_len; along++)
		INTEGER(ans_dim)[along] = count_sums[along];
	SET_DIM(ans, ans_dim);
	UNPROTECT(1);

	if (ans_len != 0) {
		ret = H5Dread(dset_id, H5T_NATIVE_INT,
			      mem_space_id, file_space_id,
			      H5P_DEFAULT, INTEGER(ans));
		H5Sclose(mem_space_id);
	}

	H5Sclose(file_space_id);
	free(count_sums);
	UNPROTECT(1);
	if (ans_len != 0 && ret < 0) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
		         "H5Dread() returned an error");
		return R_NilValue;
	}
	return ans;
}

/* --- .Call ENTRY POINT --- */
SEXP C_h5mread(SEXP filepath, SEXP name, SEXP starts, SEXP counts)
{
	SEXP filepath0, name0, ans;
	hid_t file_id, dset_id;

	/* Check 'filepath'. */
	if (!(IS_CHARACTER(filepath) && LENGTH(filepath) == 1))
		error("'filepath' must be a single string");
	filepath0 = STRING_ELT(filepath, 0);
	if (filepath0 == NA_STRING)
		error("'filepath' cannot be NA");

	/* Check 'name'. */
	if (!(IS_CHARACTER(name) && LENGTH(name) == 1))
		error("'name' must be a single string");
	name0 = STRING_ELT(name, 0);
	if (name0 == NA_STRING)
		error("'name' cannot be NA");

	file_id = H5Fopen(CHAR(filepath0), H5F_ACC_RDONLY, H5P_DEFAULT);
	if (file_id < 0)
		error("failed to open file %s", CHAR(filepath0));
	dset_id = H5Dopen(file_id, CHAR(name0), H5P_DEFAULT);
	if (dset_id < 0) {
		H5Fclose(file_id);
		error("failed to open dataset %s from file %s",
		      CHAR(name0), CHAR(filepath0));
	}
	ans = PROTECT(h5mread(dset_id, starts, counts));
	H5Dclose(dset_id);
	H5Fclose(file_id);
	if (ans == R_NilValue) {
		UNPROTECT(1);
		error(errmsg_buf);
	}
	UNPROTECT(1);
	return ans;
}



#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <assert.h>
#include <stdint.h>
#include <values.h>

#include "lib_od.h"
#include "lib_odisk.h"
#include "lib_log.h"
#include "lib_searchlet.h"
#include "attr.h"
#include "queue.h"
#include "filter_exec.h"
#include "filter_priv.h"
#include "rtimer.h"
#include "rgraph.h"
#include "rcomb.h"
#include "fexec_stats.h"

/*
 * This function walks through the list of filters and resets
 * all the statistics assocaited with each one.  This is typically
 * called when a new search is started.
 */

void
fexec_clear_stats(filter_data_t *fdata)
{
    int     i;

	for (i = 0; i < fdata->fd_num_filters; i++) {
        fdata->fd_filters[i].fi_called = 0;
        fdata->fd_filters[i].fi_drop = 0;
        fdata->fd_filters[i].fi_pass = 0;
        fdata->fd_filters[i].fi_time_ns = 0;
	}

}


int
fexec_get_stats(filter_data_t *fdata, int max, filter_stats_t *fstats)
{
	filter_info_t *	    cur_filt;
	filter_stats_t *    cur_stat;
    int                 i;


    if (fdata == NULL) {
        return(-1);
    }
	/* XXX keep the handle somewhere */
	for (i = 0; i < fdata->fd_num_filters; i++) {
        cur_filt = &fdata->fd_filters[i];

		/* if we are out of space return an error */
		if (i > max) {
			return(-1);
		}

		cur_stat = &fstats[i];

		strncpy(cur_stat->fs_name, cur_filt->fi_name, MAX_FILTER_NAME);
		cur_stat->fs_name[MAX_FILTER_NAME-1] = '\0';
		cur_stat->fs_objs_processed = cur_filt->fi_called;
		cur_stat->fs_objs_dropped = cur_filt->fi_drop;
		if (cur_filt->fi_called != 0) {
			cur_stat->fs_avg_exec_time =  
				cur_filt->fi_time_ns / cur_filt->fi_called;
		} else {
			cur_stat->fs_avg_exec_time =  0;
		}

	}
	return(0);
}


int
fexec_hash_prob(filter_id_t cur_filt, int num_prev, const filter_id_t *sorted_list)
{

        /* XXX LH XXX */
        return(0);
}



static filter_prob_t *
fexec_lookup_prob(filter_data_t *fdata, filter_id_t cur_filt, int num_prev, 
                const filter_id_t *sorted_list)
{
    int              hash;
    filter_prob_t*   cur_node;
    int              datalen;

    datalen = sizeof(filter_id_t) * num_prev;

    hash = fexec_hash_prob(cur_filt, num_prev, sorted_list);

    LIST_FOREACH(cur_node, &fdata->fd_prob_hash[hash], prob_link) {
        if ((cur_node->num_prev == num_prev) &&
            (cur_node->cur_fid == cur_filt) &&
            (memcmp(sorted_list, cur_node->prev_id, datalen) == 0)) {
                return(cur_node);
        }
    }

    return(NULL);
}



static filter_prob_t *
fexec_new_prob(filter_data_t *fdata, filter_id_t cur_filt, int num_prev, 
                filter_id_t *sorted_list)
{
    filter_prob_t*  new_node;
    int             datalen;
    int             hash;

    hash = fexec_hash_prob(cur_filt, num_prev, sorted_list);

    datalen = sizeof(filter_id_t) * num_prev;

    new_node = (filter_prob_t *) malloc(FILTER_PROB_SIZE(num_prev));
    assert(new_node != NULL);


    new_node->cur_fid = cur_filt;
    memcpy(new_node->prev_id, sorted_list, datalen);

    new_node->num_pass = 0; 
    new_node->num_exec = 0; 
    new_node->num_prev = num_prev; 

    LIST_INSERT_HEAD(&fdata->fd_prob_hash[hash], new_node, prob_link);

    return(new_node);
}



/*
 * Comparison function for qsort that compares to filter_id_t.
 */
int
id_comp(const void *data1, const void *data2)
{
    filter_id_t d1, d2;

    d1 = *(filter_id_t*)data1;
    d2 = *(filter_id_t*)data2;

    if (d1 < d2) {
        return(-1);
    } else if (d1 > d2) {
        return(1);
    } else {
        return(0);
    }
}


void
fexec_update_prob(filter_data_t * fdata,  filter_id_t cur_filt, 
		  const filter_id_t *prev_list, int num_prev, int pass)
{
    filter_id_t *   sorted_list;
    filter_prob_t * prob;

    /* we need it one larger for union stats */
    sorted_list = (filter_id_t *)malloc(sizeof(*sorted_list) * (num_prev + 1));
    assert(sorted_list != NULL);

    memcpy(sorted_list, prev_list, (num_prev * sizeof(filter_id_t)));
    
    qsort(sorted_list, num_prev, sizeof(filter_id_t), id_comp);

    /* lookup the prob data structure or allocatea  new one */
    prob = fexec_lookup_prob(fdata, cur_filt, num_prev, sorted_list);
    if (prob == NULL) {
        prob = fexec_new_prob(fdata, cur_filt, num_prev, sorted_list);
        assert(prob != NULL);
    }
    prob->num_exec++;
    if (pass) {
        prob->num_pass++;
    }
  

    /* keep the total stats for this union of items */
    sorted_list[num_prev] = cur_filt;
    qsort(sorted_list, (num_prev + 1), sizeof(filter_id_t), id_comp);
    
    prob = fexec_lookup_prob(fdata, INVALID_FILTER_ID, (num_prev + 1), 
                    sorted_list);
    if (prob == NULL) {
        prob = fexec_new_prob(fdata, INVALID_FILTER_ID, (num_prev + 1), 
                        sorted_list);
        assert(prob != NULL);
    }
    prob->num_exec++;
    if (pass) {
        prob->num_pass++;
    }
        
    return;
}


/* debug function that will probably go away */
void
fexec_dump_prob(filter_data_t *fdata)
{
    int              hash, j;
    filter_prob_t*   cur_node;

    for (hash = 0; hash < PROB_HASH_BUCKETS; hash++) {  
        LIST_FOREACH(cur_node, &fdata->fd_prob_hash[hash], prob_link) {
            fprintf(stdout, "%d -> ", cur_node->cur_fid);
            for(j=0; j<cur_node->num_prev; j++) {
                fprintf(stdout, "%d:", cur_node->prev_id[j]);
            }

            fprintf(stdout, " = pass %d total %d \n",cur_node->num_pass,
                                cur_node->num_exec);
        }
    }

}




#ifdef  XXX
int
fexec_update_cost(finfo_filter, int pass)
{
	filter_info_t *	cur_filt;
	filter_stats_t * cur_stat;
	int		cur_num;


	cur_filt = finfo;
	cur_num = 0;

	/* XXX keep the handle somewhere */
	while (cur_filt != NULL) {
		/* if we are out of space return an error */
		if (cur_num > max) {
			return(-1);
		}

		cur_stat = &fstats[cur_num];

		strncpy(cur_stat->fs_name, cur_filt->fi_name, MAX_FILTER_NAME);
		cur_stat->fs_name[MAX_FILTER_NAME-1] = '\0';
		cur_stat->fs_objs_processed = cur_filt->fi_called;
		cur_stat->fs_objs_dropped = cur_filt->fi_drop;
		if (cur_filt->fi_called != 0) {
			cur_stat->fs_avg_exec_time =  
				cur_filt->fi_time_ns / cur_filt->fi_called;
		} else {
			cur_stat->fs_avg_exec_time =  0;
		}


		/* update the number counter and the cur filter pointer */
		cur_num++;	
		cur_filt = cur_filt->fi_next;
	}

	return(0);
}

#endif /* XXX_LH */

#define SIGNIFICANT_NUMBER(g) ((g)*8)

/* evaluate a permutation in the context of the currently available
 * data, and return a utility value (higher is better).
 * the function value is non-zero on error
 */
int
fexec_evaluate(filter_data_t *fdata, permutation_t *perm, int gen, int *utility) {
  int err=0;
  int i;
  filter_prob_t *fprob;
  double pass = 1;		/* cumul pass rate */
  double totalcost = 0;		/* = utility */
  filter_info_t *info;
  char buf[BUFSIZ];
  int n;

  /* NB: this assumes that filter_id_t and pelt_t are the same type XXX */
  assert(sizeof(pelt_t) == sizeof(filter_id_t));

  printf("fexec_evaluate: %s\n", pmPrint(perm, buf, BUFSIZ));

  for(i=0; i < pmLength(perm); i++) {
    double c;			/* cost of this filter */
    double p;			/* pass rate for this filter in this pos */
    /* pass = pass rate of all filters before this one */
    info = &fdata->fd_filters[pmElt(perm, i)];
    c = info->fi_time_ns;
    n = info->fi_called;
    if(n < SIGNIFICANT_NUMBER(gen)) {
      return 1;
    }

    totalcost += pass * c / n;		/* prev cumul pass * curr cost */
    printf("\tpass=%f, cst=%f, total=%f\t", pass, c/n, totalcost);

    /* lookup this permutation */
    fprob = fexec_lookup_prob(fdata, pmElt(perm, i), i, pmArr(perm)); /* XXX */
    if(fprob) {
      p = (double)fprob->num_pass / fprob->num_exec;
      printf("\t(cond p=%f)", p);
#if 0
    } else if(n > SIGNIFICANT_NUMBER(gen)) {
      /* permutation not found, try assuming independence */
      p = 1.0 - (double)info->fi_drop / n;
      printf("\t(indp p=%f)", p);
#endif
    } else {
      /* really no data, return an error */
      //printf("fexec_evaluate: could not evaluate permutation, giving up\n");
      return 1;
      //p = 1;			/* cost will be 0 anyway */
      //n = 1;
    }
    printf("\n");

    assert(p >= 0 && p <= 1.0);
    pass *= p;
#define SMALL_FRACTION (0.00001)
    /* don't let it go to zero XXX */
    if(pass < SMALL_FRACTION) {
      pass = SMALL_FRACTION;
    }
  }

  *utility = -totalcost;
  //assert(*utility >= 0);

  //printf("fexec_evaluate: %s = %d\n", pmPrint(perm, buf, BUFSIZ), *utility);
  printf("fexec_evaluate: ");
  fexec_print_cost(fdata, perm);
  printf(" cost=%s\n", format_number(buf, totalcost));
  return err;
}

/* not used? */
/* int */
/* fexec_single(filter_data_t *fdata, int fid, int *utility) { */
/*   filter_info_t *info; */
/*   double c, n, p; */
/*   double wc; */
/*   int err = 0; */

/*   info = &fdata->fd_filters[fid]; */
/*   n = info->fi_called; */
/*   if(!n) n = 1;		/\* avoid div 0 *\/ */
/*   c = info->fi_time_ns / n; */
/*   p = 1.0 - (double)info->fi_drop / n; */

/*   wc = p * c; */

/*   *utility = (INT_MAX>>4) - wc; */
/*   assert(*utility >= 0); */
/*   return err; */
/* } */




/* debug function */
void
fexec_print_cost(const filter_data_t *fdata, const permutation_t *perm) {
  int i;
  const filter_info_t *info;
  double c, p;
  int n;
  char buf[BUFSIZ];

  printf("[");
  for(i=0; i < pmLength(perm); i++) {
    info = &fdata->fd_filters[pmElt(perm, i)];
    c = info->fi_time_ns;
    n = info->fi_called;
    if(!n) n = 1;		/* avoid div 0 */
    p = info->fi_called - info->fi_drop;
    printf(" %d=c%s,p%.0f/%d", pmElt(perm, i), format_number(buf, c/n), p, n);
  }
  printf(" ]");
}


#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "obj_attr.h"
#include "lib_odisk.h"
/* #include "lib_search_priv.h" */

#define	DIR_PATH_NAME	"/opt/dir1"
#define	MAX_FNAME	128





static int
odisk_open_dir(void **cookie)
{
	DIR *			dir;

	dir = opendir(DIR_PATH_NAME);
	if (dir == NULL) {
		/* XXX log */
		printf("failed to open %s \n", DIR_PATH_NAME);
		return(ENOENT);
	}

	*cookie = (void *)dir;

	return(0);
}


int
odisk_load_obj(obj_data_t  **obj_handle, char *name)
{
	obj_data_t *	new_obj;
	struct stat	stats;
	FILE	 *	os_file;
	char *		data;
	int		err;
	size_t		size;


	/* XXX printf("load_obj: <%s> \n", name); */

	if (strlen(name) >= MAX_FNAME) {
		/* XXX log error */
		return (EINVAL);
	}

	new_obj = malloc(sizeof(*new_obj));
	if (new_obj == NULL) {
		/* XXX log error */
		return (ENOMEM);
	}

	err = stat(name, &stats);
	if (err != 0) {
		free(new_obj);
		return(ENOENT);
	}
		

	/* open the file */
	os_file  = fopen(name, "rb");
	if (os_file == NULL) {
		free(new_obj);
		return (ENOENT);
	}

	data = (char *)malloc(stats.st_size);
	if (data == NULL) {
		fclose(os_file);
		free(new_obj);
		return (ENOENT);

	}
	
	size = fread(data, stats.st_size, 1, os_file);
	if (size != 1) {
		/* XXX log error */
		printf("failed to read data \n");
		free(data);
		fclose(os_file);
		free(new_obj);
		return (ENOENT);
	}

	new_obj->data = data;
	new_obj->data_len = stats.st_size;
	new_obj->attr_info.attr_data = NULL; /* XXX */
	new_obj->attr_info.attr_len = 0; /* XXX */
	
	*obj_handle = (obj_data_t *)new_obj;

	return(0);
}



int
odisk_next_obj(obj_data_t **new_object, odisk_state_t *odisk)
{
	DIR *			dir;
	struct dirent *		cur_ent;
	char			path_name[NAME_MAX];
	int			err;
	int			extlen, flen;
	char *			poss_ext;

	dir = (DIR *)odisk->iter_cookie;

next:
	cur_ent = readdir(dir);
	if (cur_ent == NULL) {
		/* printf("no ent !! \n"); */
		return(ENOENT);
	}

	/*
	 * If this isn't a file then we skip the entry.
	 */
	if (cur_ent->d_type != DT_REG) {
		/* printf("not regular file %s \n", cur_ent->d_name); */
		goto next;
	}

	/*
	 * If this entry ends with the string defined by ATTR_EXT,
	 * then this is not a data file but an attribute file, so
	 * we skip it.
	 */
	extlen = strlen(ATTR_EXT);
	flen = strlen(cur_ent->d_name);
	if (flen > extlen) {
		poss_ext = &cur_ent->d_name[flen - extlen];
		if (strcmp(poss_ext, ATTR_EXT) == 0) {
			goto next;
		}
	}

	sprintf(path_name, "%s/%s", DIR_PATH_NAME, cur_ent->d_name);

	err = odisk_load_obj(new_object, path_name);
	if (err) {
		/* XXX log */
		printf("create obj failed %d \n", err);
		return(err);
	}

	return(0);
}



int
odisk_init(odisk_state_t **odisk)
{
	int	err;
	odisk_state_t  *	new_state;

	new_state = (odisk_state_t *)malloc(sizeof(*new_state));
	if (new_state == NULL) {
		/* XXX err log */
		return(ENOMEM);
	}
	

	err = odisk_open_dir(&new_state->iter_cookie);
	if (err != 0) {
		free(new_state);
		/* XXX log */
		printf("failed to init device emulation \n");
		return (err);
	}
	*odisk = new_state;
	return(0);
}




int
odisk_term(odisk_state_t *odisk)
{
	DIR *	dir;
	int	err;

	dir = (DIR *)odisk->iter_cookie;

	err = closedir(dir);

	odisk->iter_cookie = NULL;

	free(odisk);
	return (err);
}

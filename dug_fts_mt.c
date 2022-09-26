#include<stdio.h>
#include<stdbool.h>
#include<stdlib.h>
#include<dirent.h>
#include<limits.h>
#include<errno.h>
#include<string.h>
#include<unistd.h>
#include<grp.h>
#include<fts.h>
#include<pthread.h>
#include<sys/stat.h>

#define MAXGIDS    128
#define MAXPATHLEN 2048

extern errno;

bool  verbose = false, json = false, output_names = false, size_in_blocks = false;
int   max_errors = 128, n_errors = 0;
char  **error_strs;

struct tr_args {
    char* path;
    int** n_results;
    unsigned long long **data;
};

int parse_num(char* arg) {
    errno = 0;
    char* end;
    long i = strtol(arg, &end, 10);
    if(arg == end)
        return -1;

    if(errno == ERANGE)
        return -1;

    if(i > INT_MAX)
       return -1;

    return (int)i; 
}

int find_index(int gid, unsigned int gids[]) {
    int index = gid % MAXGIDS;
    int i = index;
  
    do {
        if(gids[i] == gid)
            return i;
        else if(gids[i] == UINT_MAX) {
            gids[i] = gid;
            return i;
        }
        i = (i+1)%MAXGIDS;
    } while(i!=index);
    
    // Table is full and does not contain
    // an entry for the query GID
    return -1;
}

int get_name(unsigned int gid, char* name) {
    struct group *grp = getgrgid(gid); 
    if(grp == NULL)
        sprintf(name, "%u", gid);
    else
        sprintf(name, "%s", grp->gr_name);
    return 0;
}

int insert_or_update(unsigned int gid, long long unsigned int size, unsigned int gids[], long long unsigned int sizes[]) {
    int index = find_index(gid, gids);
    if(index == -1)
        return 1;
    sizes[index] += size;
    return 0;
}

int store_error(char* path, char* error) {
    if(n_errors >= max_errors)
        return 1;

    int total_length = strlen(path)+strlen(error);
    error_strs[n_errors] = malloc(total_length+10);
    sprintf(error_strs[n_errors], "%s: %s", path, error);
    n_errors++;
    return 0;
}

int read_dir(char* paths[], int** n_results, unsigned long long **data) {
    FTS *stream;
    FTSENT *entry;
    int i, j;
    bool insert = false;
    bool error = false;
    long long unsigned int audit_size;
    long long unsigned int sizes[MAXGIDS];
    unsigned int gids[MAXGIDS];

    stream = fts_open(paths, FTS_PHYSICAL, NULL);
    if(stream == NULL) {
        store_error(paths[0], strerror(errno));
        return 1; 
    }

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    while((entry=fts_read(stream))) {
        if(entry == NULL) {
            store_error("fts_error", strerror(errno));
            continue;
        }

        // Process the file or directory
        insert = false;
        error = false;
        switch(entry->fts_info) {
            case FTS_F:
                if(verbose)
                    printf("+file      %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_D:
                if(verbose)
                    printf("+directory %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_SL:
                if(verbose)
                    printf("+symink    %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_SLNONE:
                if(verbose)
                    printf("+brksymink %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_DEFAULT:
                if(verbose)
                    printf("+uncat     %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_DNR:
                error = true;
                break;
            case FTS_DP:
                // We already saw this directory in preorder FTS_D,
                // so ignore it
                break;
            case FTS_NS:
                if(verbose)
                    printf("Could not stat file %s\n", entry->fts_path);
                error = true;
                break;
            case FTS_ERR:
                if(verbose)
                    printf("FTS error accessing %s\n", entry->fts_path);
                error = true;
                break;
            default: 
                if(verbose)
                    printf("Skipping %s\n", entry->fts_path);
        }

        // Update the running usage in the hash table
        if(insert) {
            // Compute size as either file size, or size of
            // blocks the file spans
            audit_size = entry->fts_statp->st_size;
            if(size_in_blocks)
                audit_size = entry->fts_statp->st_blocks*512;
 
            if(insert_or_update(entry->fts_statp->st_gid, audit_size, gids, sizes) != 0) {
                store_error(entry->fts_path, "GID table overflowed");
                return 2;
            }
        }

        // Store error, and exit if maximum errors reached 
        if(error) {
            if(store_error(entry->fts_path, strerror(entry->fts_errno)) != 0) {
                return 3;
            }
        }
    }

    // Compute number of groups encountered 
    *n_results = malloc(sizeof(unsigned int));
    **n_results = 0; 
    for(i=0;i<MAXGIDS;i++) {
        if(gids[i] != UINT_MAX)
            **n_results += 1;
    }

    // Store the results
    *data = calloc(**n_results*2, sizeof(long long unsigned int));
    j = 0;
    for(i=0;i<MAXGIDS;i++) {
        if(gids[i] != UINT_MAX) {
            (*data)[j] = gids[i];
            (*data)[j+1] = sizes[i];
            j += 2; 
        }
    }

    fts_close(stream);
    return 0;
}

int get_n_subdirs(char* path, unsigned int *n_subdirs) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    int i, status;
    char* temppath = malloc(MAXPATHLEN);
    unsigned int sd = 0;

    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        return 1;
    }

    while((entry=readdir(dp))) {
        if(strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0)
            continue;

        sprintf(temppath, "%s/%s", path, entry->d_name);
        if(lstat(temppath, &meta) != 0) {
            store_error(temppath, " Could not stat file");
            continue;
        }

        switch(meta.st_mode & S_IFMT) {
            case S_IFDIR:
                sd += 1;
        }
    }
 
    *n_subdirs = sd;
    closedir(dp);
    free(temppath);
    return 0;
}

int find_slot_tr(pthread_t *thread_ids[], unsigned int max_n_threads) {
    int i, status;
    int slot = -1;
    while(slot == -1) {
        for(i=0;i<max_n_threads;i++) {
            if(thread_ids[i] == NULL)
                return i;
        }
        printf("Waiting for thread slot to become available\n");
        for(i=0;i<max_n_threads;i++) {
            status = pthread_tryjoin_np(*thread_ids[i], NULL);
            if(status == 0)
                thread_ids[i] = NULL;
        }
        sleep(1); 
    }
}

int walk(char* path, unsigned int max_n_threads) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    int i, status, thread_i;
    char* temppath = malloc(MAXPATHLEN);
    bool insert, process;
    long long unsigned int audit_size;
    long long unsigned int sizes[MAXGIDS];
    unsigned int gids[MAXGIDS];
    unsigned int n_subdirs = 0;
    
    if((status=get_n_subdirs(path, &n_subdirs)) != 0)
        return 1;

    struct tr_args descendents[max_n_threads];
    pthread_t *thread_ids[max_n_threads];

    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        return 1;
    }

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    // Fill thread ID pointers with NULL
    for(i=0;i<max_n_threads;i++)
        thread_ids[i] = NULL;

    while((entry=readdir(dp))) {
        // Skip navigational entries
        if(strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0)
            continue;

        // Get the file metadata
        sprintf(temppath, "%s/%s", path, entry->d_name);
        if(lstat(temppath, &meta) != 0) {
            store_error(temppath, " Could not stat file");
            continue;
        }

        // Process the file or directory
        process = insert = false;
        switch(meta.st_mode & S_IFMT) {
            case S_IFLNK:
                if(verbose)
                    printf("Not following symlink %s\n", temppath);
                insert = true; 
            case S_IFREG:
                if(verbose)
                    printf("Processing regular file %s\n", temppath);
                insert = true;
                break;
            case S_IFDIR:
                if(verbose)
                    printf("Processing directory %s\n", temppath);
                insert = process = true;
            default:
                if(verbose)
                    printf("Skipping file %s\n", temppath);
        }

        // Update the running usage in the hash table
        if(insert) {
            // Compute size as either file size, or size of
            // blocks the file spans
            audit_size = meta.st_size;
            if(size_in_blocks)
                audit_size = meta.st_blocks*512;

            if(insert_or_update(meta.st_gid, audit_size, gids, sizes) != 0) {
                store_error(temppath, "GID table overflowed");
                return 2;
            }
        }


        // If it is a subdirectory, we need to launch a thread to process it
        if(process) {
             printf("Launch a thread to process file %s\n", temppath);
             thread_i=find_slot_tr(thread_ids, max_n_threads);
             thread_ids[thread_i] = 1;
        }
    }

    free(temppath);
    closedir(dp);
    return 0;
}

int usage() {
    printf("USAGE: dug [OPTIONS] <directory>\n\n");
    printf("OPTIONS\n");
    printf("    -b  Compute size of blocks occupied (default is file sizes)\n");
    printf("    -h  Display help information\n");
    printf("    -j  Output result in JSON format (default is plain text)\n");
    printf("    -m  Maximum errors before terminating (default is 128)\n");
    printf("    -n  Output group names (default output uses gids)\n");
    printf("    -v  Output information about each file encountered\n");
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    int i;
    char *paths[2] = {NULL, NULL};   
    char c; 
    int* n_results;
    long long unsigned int *data;

    //struct op_args t1 = {.path = NULL, .n_results = malloc(sizeof(int*)), .data = malloc(sizeof(long long unsigned int*))};

    // If run with no arguments, output usage
    if(argc < 2) 
        return usage();

    // Parse arguments
    while((c = getopt(argc, argv, "hjvnbm:")) != -1) {
        switch(c) {
            case 'm':
                max_errors = parse_num(optarg);
                if(max_errors < 0 || max_errors > 65535) {
                    printf("Value for -m %s was not in range [0,65535]\n", optarg);
                    return 1;
                }
                break;
            case 'v':
                verbose = true;
                break;
            case 'j':
                json = true;
                break;
            case 'n':
	        output_names = true;
	        break;
            case 'b':
                size_in_blocks = true;
                break; 
            case 'h':
                usage();
                return 0;
        }
    }

    // Parse path, or exit if non specified
    if (optind >= argc) {
        printf("Path argument is required! Review usage with -h\n");
        return 1;
    }
    paths[0] = argv[optind];
    if(verbose)
        printf("Auditing directory %s\n", paths[0]);

    // Initialize error string to requested
    // number of pointers
    error_strs = malloc(max_errors*sizeof(char*));

    // Compile the usage by group under path
    i = walk(paths[0], 2);
    if(i == 2) {
        printf("The number of groups encountered exceeded the maximum value of %u\n", MAXGIDS);
        return 2;
    }
    else if(i == 3) {
        printf("The maximum number of errors %d was exceeded\n", max_errors);
        return 3;
    }

    //printf("N Groups: %u\n", *n_results);
    //for(i=0;i<*n_results*2;i+=2) {
    //    printf("%llu\t%llu\n", data[i], data[i+1]);
    //}
    
    // Cleanup
    for(i=0;i<n_errors;i++) {
        free(error_strs[i]);
    }
    free(error_strs);
    free(n_results);
    free(data);

    return 0;
}


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
#include<sys/stat.h>

#define MAXGIDS 128

extern errno;
long long unsigned int sizes[MAXGIDS];
unsigned int gids[MAXGIDS];
bool verbose = false;
bool json = false;
bool output_names = false;
bool size_in_blocks = false;
int max_errors = 128;
int n_errors = 0;
char** error_strs;


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

int find_index(int gid) {
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

int insert_or_update(unsigned int gid, long long unsigned int size) {
    int index = find_index(gid);
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

int read_dir(char* paths[]) {
    FTS *stream;
    FTSENT *entry;
    bool insert = false;
    bool error = false;
    long long unsigned int audit_size;

    stream = fts_open(paths, FTS_PHYSICAL, NULL);
    if(stream == NULL) {
        store_error(paths[0], strerror(errno));
        return 1; 
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
 
            if(insert_or_update(entry->fts_statp->st_gid, audit_size) != 0) {
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
    fts_close(stream);
    return 0;
}

int output_json() {
    int i;
    int out = 0;
    unsigned long long int total = 0;
    char* name_buffer = malloc(2048);
    printf("{\n  \"errors\": [\n");

    for(i=0;i<n_errors;i++) {
        if(i > 0)
            printf(",\n");
        printf("    \"%s\"", error_strs[i]);
    }
    printf("\n  ],\n  \"usage\": {\n");
    for(i=0;i<MAXGIDS;i++) {
        if(gids[i] != UINT_MAX) {
            if(out > 0)
                printf(",\n");
	    if(output_names)
                get_name(gids[i], name_buffer);
            else
		sprintf(name_buffer, "%u", gids[i]);
            printf("    \"%s\":%llu", name_buffer, sizes[i]);
            total += sizes[i];
            out++;
        }
    }
    if(out > 0)
        printf(",\n");
    printf("    \"total\":%llu", total);
    printf("\n  }\n}\n");    
    free(name_buffer);
    return 0;
}

int output_table() {
    int i;
    char* name_buffer = malloc(2048);

    if(n_errors > 0) {
        printf("==================== Errors ====================\n");
        for(i=0;i<n_errors;i++) {
            printf("%s\n", error_strs[i]);
        }
    }

    printf("\n==================== USAGE =====================\n");
    for(i=0;i<MAXGIDS;i++) {
        if(gids[i] != UINT_MAX) {
            if(output_names)
                get_name(gids[i], name_buffer);
            else
                sprintf(name_buffer, "%u", gids[i]);
            printf("%20s %llu\n", name_buffer, sizes[i]);
        }
    }
   
    free(name_buffer);
    return 1;
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

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0; 
    }

    // Initialize error string to requested
    // number of pointers
    error_strs = malloc(max_errors*sizeof(char*));

    // Compile the usage by group under path
    i = read_dir(paths);
    if(i == 2) {
        printf("The number of groups encountered exceeded the maximum value of %u\n", MAXGIDS);
        return 2;
    }
    else if(i == 3) {
        printf("The maximum number of errors %d was exceeded\n", max_errors);
        return 3;
    }

    // Output result
    if(json)
        output_json();
    else
        output_table();

    // Cleanup
    for(i=0;i<n_errors;i++) {
        free(error_strs[i]);
    }
    free(error_strs);

    return 0;
}


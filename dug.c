#include<stdio.h>
#include<stdbool.h>
#include<stdlib.h>
#include<dirent.h>
#include<limits.h>
#include<errno.h>
#include<string.h>
#include<unistd.h>
#include<grp.h>
#include<sys/stat.h>

#define MAXGIDS 128

extern errno;
long long unsigned int sizes[MAXGIDS];
unsigned int gids[MAXGIDS];
bool verbose = false;
bool json = false;
bool output_names = false;
int max_errors = 10;
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

int findIndex(int gid) {
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

int insertOrUpdate(unsigned int gid, long long unsigned int size) {
    int index = findIndex(gid);
    if(index == -1)
        return 1;
    sizes[index] += size;
    return 0;
}

int store_error(char* path, char* error) {
    int total_length = strlen(path)+strlen(error);
    error_strs[n_errors] = malloc(total_length+10);
    sprintf(error_strs[n_errors], "%s: %s", path, error);
    n_errors++;
}

int read_dir(char* path) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    int i, status; 
    char* temppath = malloc(2048);


    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        return 1; 
    }

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
        switch(meta.st_mode & S_IFMT) {
            case S_IFLNK:
                if(verbose)
                    printf("Not following symlink %s\n", temppath);
                continue;
            case S_IFREG:
                if(verbose)
                    printf("Processing regular file %s\n", temppath);
                if(insertOrUpdate(meta.st_gid, meta.st_size) != 0) {
                    store_error(temppath, "GID table overflowed");
                    free(temppath);
                    return 2;
                }
                break;
            case S_IFDIR:
                if(verbose)
                    printf("Processing directory %s\n", temppath);
                if((status=read_dir(temppath)) == 2) {
                    free(temppath);
                    return status;
                }
                break;
            default: 
                if(verbose)
                    printf("Skipping special file %s\n", temppath);
        }
    }

    closedir(dp);
    free(temppath);
    return 0;
}

int output_json() {
    int i;
    int out = 0;
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
            out++;
        }
    }
    printf("\n  }\n}\n");    
    free(name_buffer);
    return 0;
}

int usage() {
    printf("USAGE: dug [OPTIONS] <directory>\n");
    return 0;
}

int main(int argc, char** argv) {
    int i;
    char* path;   
    char c; 

    // If run with no arguments, output usage
    if(argc < 2) 
        return usage();

    // Parse arguments
    while((c = getopt(argc, argv, "hjvnm:")) != -1) {
        switch(c) {
            case 'm':
                max_errors = parse_num(optarg);
                if(max_errors < 0) {
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
    path = argv[optind]; 

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0; 
    }

    // Initialize error string to requested
    // number of pointers
    error_strs = malloc(max_errors*sizeof(char*));


    // Compile the usage by group under path
    read_dir(path);

    // Output result
    output_json();

    // Cleanup
    for(i=0;i<n_errors;i++) {
        free(error_strs[i]);
    }
    free(error_strs);

    return 0;
}


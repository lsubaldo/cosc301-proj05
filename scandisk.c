#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

//Leslie's comments/notes  
//can't share clusters --> can remove directory entry or copy and paste it into another cluster 
//sector, block, and cluster are synonymous 
//if entry in FAT, but not in refs, SAVE THE ORPHANS!! DON'T MARK AS FREE IN FAT - WILL KILL ORPHAN; make new directory entry 

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}


/* write the values into a directory entry */
void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
	if (p2[i] == '/' || p2[i] == '\\') 
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}


/* create_dirent finds a free slot in the directory, and write the
   directory entry */

void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, uint32_t size,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, size);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
	    return;
	}
	dirent++;
    }
}


// modified so that it can detect size inconsistencies 
uint16_t print_dirent(struct direntry *dirent, int indent, uint8_t *image_buf, struct bpb33 *bpb, int *clustrefs)
{
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY)
    {
	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	// ignore any long file name extension entries
	//
	// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {
	printf("Volume: %s\n", name);
    } 
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
    {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
        {
	    print_indent(indent);
    	    printf("%s/ (directory)\n", name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    }
    else 
    {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
		int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
		int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
		int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
		int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

		size = getulong(dirent->deFileSize);
		print_indent(indent);
		printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
			   name, extension, size, getushort(dirent->deStartCluster),
			   ro?'r':' ', 
		       hidden?'h':' ', 
		       sys?'s':' ', 
		       arch?'a':' ');

		/*Checking cluster chain*/ 
		int num_clusters = 0; //cluster chain count 
		uint16_t cluster = getushort(dirent->deStartCluster);
		uint16_t first_clust = cluster;
		uint16_t prev;
		while (is_valid_cluster(cluster, bpb)) {
			clustrefs[cluster]++;
			if (clustrefs[cluster] > 1) {
				dirent->deName[0] = SLOT_DELETED;
				clustrefs[cluster]--;
				printf("Inconsistent! Multiple references to the same cluster.\n"); 
			}
			prev = cluster;
			cluster = get_fat_entry(cluster, image_buf, bpb);
			if (prev == cluster) {
				printf("Cluster points to itself.\n");
				//set_fat_entry(cluster, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
				num_clusters++;
				break;
			}
			if (cluster == (FAT12_MASK & CLUST_BAD)) {
				printf("CLUSTER IS BAD.\n");
				/*set_fat_entry(cluster, FAT12_MASK & CLUST_FREE, image_buf, bpb);
				set_fat_entry(prev, FAT12_MASK & CLUST_EOFS, image_buf, bpb);*/
				num_clusters++;
				break;
				//is it possible for a cluster to be both 'BAD' and have a size inconsistency?? YES  
			}
			num_clusters++;
		}
		int meta_count = 0; //number of clusters, according to the metadata
		//int rem = 0;
		uint32_t new_size = 0;
		if (size%512 == 0) {
			meta_count = size/512;
		}
		else {
			meta_count = (size/512) + 1;
			//rem = size%512;
			//printf("remainder is: %d\n", rem); 
		}
		printf("meta_count is: %d\n", meta_count);
		printf("num_clusters is %d\n", num_clusters);
		if (meta_count < num_clusters) {
			printf("INCONSISTENCY. File size less than number of clusters in FAT.\n"); 
			//free any clusters that are beyond the end of a file, but to which the FAT chain still points 
			/*cluster = get_fat_entry(first_clust + meta_count - 1, image_buf, bpb);
			while (is_valid_cluster(cluster, bpb)) {
				prev = cluster;
				set_fat_entry(prev, FAT12_MASK & CLUST_FREE, image_buf, bpb);
				cluster = get_fat_entry(cluster, image_buf, bpb);
			}
			set_fat_entry(first_clust + meta_count - 1, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
			uint32_t diff = (num_clusters * bpb->bpbBytesPerSec) - size; 
			printf("diff is %d\n", diff);
			new_size = size + bpb->bpbBytesPerSec; 
			printf("new_size is: %d\n", new_size); 
			putulong(dirent->deFileSize, new_size); 
			new_size = num_clusters * bpb->bpbBytesPerSec;
			printf("New size is: %d\n", new_size);
			printf("Previous size is: %d\n", size); */
		}
		else if (meta_count > num_clusters) {
			printf("INCONSISTENCY. File size greater than number of clusters in FAT.\n"); 
			new_size = num_clusters * bpb->bpbBytesPerSec;
			putulong(dirent->deFileSize, new_size);
		}
	
    }

    return followclust;
}


void follow_dir(uint16_t cluster, int indent, uint8_t *image_buf, struct bpb33* bpb, int *clustrefs)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
            
            uint16_t followclust = print_dirent(dirent, indent, image_buf, bpb, clustrefs);
            if (followclust) {
				clustrefs[followclust]++;
                follow_dir(followclust, indent+1, image_buf, bpb, clustrefs);
			}
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb, int *clustrefs)
{
	printf("HEY, I'M HERE. I'M TRAVERSING.\n"); 
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = print_dirent(dirent, 0, image_buf, bpb, clustrefs);
        if (is_valid_cluster(followclust, bpb)) {
			clustrefs[followclust]++;
            follow_dir(followclust, 1, image_buf, bpb, clustrefs);
		}

        dirent++;
    }
}


// Find orphans and save them from their doom 
void save_orphans(uint8_t *image_buf, struct bpb33 *bpb, int *clustrefs) {
	printf("Looking for orphans.\n"); 
	//struct direntry *dirent = (struct direntry*)cluster_to_addr(0, image_buf, bpb); -- need to create directory entry for orphans at some point in this code 
	int orphans = 0;
	for (int i = 2; i < bpb->bpbSectors; i++) {
		uint16_t cluster = get_fat_entry(i, image_buf, bpb); 
		if (clustrefs[i] == 0 && cluster != (FAT12_MASK & CLUST_FREE)) {
			printf("Orphan found at %d. We must save it.\n", i); 
			orphans++;
		}
	}
	printf("Found %d orphan(s).\n", orphans); 
	
}


void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}


int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
		usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...

	//Structure that keeps track of clusters 
	int *clustrefs = malloc(sizeof(int) * bpb->bpbSectors);
	for (int i=0; i<bpb->bpbSectors; i++) {
		clustrefs[i] = 0;
	}
 
	traverse_root(image_buf, bpb, clustrefs); 
	save_orphans(image_buf, bpb, clustrefs); 
	
	//badimage5.img problem: some clusters incremented to 2; must fix somewhere
	for (int j=0; j<bpb->bpbSectors; j++) {
		if (clustrefs[j] > 1) {
			printf("num in cluster %d is: %d\n", j, clustrefs[j]); 
		}
	}
	printf("num of sectors is: %d\n", bpb->bpbSectors); 
	printf("sector size is: %d\n", bpb->bpbBytesPerSec);	
    unmmap_file(image_buf, &fd);
    return 0;
}

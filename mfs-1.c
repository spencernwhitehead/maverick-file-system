/*

	Name: Spencer Whitehead
	ID: 1001837401
	
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

#define WHITESPACE " \t\n"		// We want to split our command line up into tokens
								// so we need to define what delimits our tokens.
								// In this case  white space
								// will separate the tokens on our command line

#define MAX_COMMAND_SIZE 255	// The maximum command-line size

#define MAX_NUM_ARGUMENTS 5	// Mav shell only supports four arguments


#define NUM_BLOCKS 4226
#define BLOCK_SIZE 8192
#define MAX_FILE_SIZE 10240000
#define MAX_NUM_FILES 125
#define MAX_FILENAME 32
#define MAX_BLOCKS_PER_FILE MAX_FILE_SIZE / BLOCK_SIZE

unsigned char file_data[NUM_BLOCKS][BLOCK_SIZE];
//functions as free block map since it can be iterated through to find next free block
int * used_blocks;

//keeps track of blocks from most recently deleted file
int del_blocks[MAX_BLOCKS_PER_FILE];

//keeps track of name of current open filesystem
char open_fs[MAX_FILENAME];

struct directory_entry {
	char name[MAX_FILENAME];
	int valid;
	int inode_idx;
	int h;
	int r;
};

//will contain array of all in-use inodes
//stored in blocks 0-1
struct directory_entry * directory_ptr;

struct inode {
	time_t date;  
	int valid;
	int size;
	int blocks[MAX_BLOCKS_PER_FILE];
};

//array of all inodes
//functions as free inode map since it can be iterated through to find next free inode
struct inode * inode_array_ptr[MAX_NUM_FILES];

//returns free space on disk by summing up all free blocks and multiplying by block size
int df() {
	int count = 0;
	int i = 0;
	for(i = 130; i < NUM_BLOCKS; i++) {
		if(used_blocks[i] == 0) {
			count++;
		}
	}
	return count * BLOCK_SIZE;
}   

//returns next available free block from an inode
int findFreeInodeBlockEntry(int inode_index) {
	int i;
	int retval = -1;
	for(i = 0; i < MAX_BLOCKS_PER_FILE; i++) {
		if(inode_array_ptr[inode_index]->blocks[i] == -1) {
			retval = i;
			break;
		}
	}
	return retval; 
}

//sets up filesystem structure for initial use
void init() {
	int i;
	//sets directory pointer to block 0
	directory_ptr = (struct directory_entry*) &file_data[0];
	
	//sets all entries in directory to not valid, since there are no entries
	for(i = 0; i < MAX_NUM_FILES; i++) {
		directory_ptr[i].valid = 0;
		directory_ptr[i].h = 0;
		directory_ptr[i].r = 0;
	}
	
	//TODO set free inode map to block 2 if needed
	
	//set free block map to block 3 and set all values to 0
	used_blocks = (int *) &file_data[3];
	for(i = 130; i < NUM_BLOCKS; i++) {
		used_blocks[i] = 0;
	}
	
	
	//assigns inode to each inode block
	int inode_idx = 0;
	for(i = 5; i < 130; i++) {
		inode_array_ptr[inode_idx++] = (struct inode*) &file_data[i];
		
		//sets all block indexes in the inode to -1
		int j;
		for(j = 0; j < MAX_BLOCKS_PER_FILE; j++) {
			inode_array_ptr[inode_idx-1]->blocks[j] = -1;
		}
	}
	
	//sets deleted blocks to be all -1
	for(i = 0; i < MAX_BLOCKS_PER_FILE; i++) {
		del_blocks[i] = -1;
	}
}

//returns index of next free entry in the directory
int findFreeDirectoryEntry() {
	int i;
	int retval = -1;
	for(i = 0; i < MAX_NUM_FILES; i++) {
		if(directory_ptr[i].valid == 0) {
			retval = i;
			break;
		}
	}
	return retval; 
}

//returns next free inode not in use
int findFreeInode() {
	int i;
	int retval = -1;
	for(i = 0; i < MAX_NUM_FILES; i++) {
		if(inode_array_ptr[i]->valid == 0) {
			retval = i;
			break;
		}
	}
	return retval;
} 

//returns next free block not in use
int findFreeBlock() {
	int retval = -1;
	int i = 0;
	
	for(int i = 130; i < NUM_BLOCKS; i++) {
		if(used_blocks[i] == 0) {
			retval = i;
			break;
		}
	}
	return retval;
}

//takes file <filename> and stores it in blocks into the filesystem
void put(char * filename) {
	struct stat buf; 
	
	//stores whether file exists, and gives buf the file size
	//along with other stuff we dont use
	int status = stat(filename, &buf);
	
	//if status = -1, no file was found
	if(status == -1) {
		printf("error: file not found\n");
		return;
	}
	
	//if file size is greater than free space in the system, it cant be stored
	if(buf.st_size > df()) {
		printf("error: not enough room in file system\n");
		return;
	}
	
	//if there are no more free directory entries, the file cant be stored
	int dir_idx = findFreeDirectoryEntry();
	if(dir_idx == -1) {
		printf("error: not enough room in file system\n");
		return;
	}
	
	//set directory to be in use and set directory name to filename
	directory_ptr[dir_idx].valid = 1;
	strncpy(directory_ptr[dir_idx].name, filename, strlen(filename));
	
	//get free inode index and check if valid, if valid then assign to directory
	int inode_idx = findFreeInode();
	if(inode_idx == -1) {
		printf("error: no free inodes\n");
		return;
	}
	directory_ptr[dir_idx].inode_idx = inode_idx;
	
	//set all the info for the inode
	inode_array_ptr[inode_idx]->size = buf.st_size;
	inode_array_ptr[inode_idx]->date = time(NULL);
	inode_array_ptr[inode_idx]->valid = 1;
	time(&(inode_array_ptr[inode_idx]->date));
	
	//open file for grabbing info from
	FILE *ifp = fopen(filename, "r");
	
	int copy_size = buf.st_size;
	int offset = 0;
	
	//stores data from file in blocks and assigns them to the inode
	while(copy_size >= BLOCK_SIZE) {
		//get block for storing, error check and set to be in use
		int block_index = findFreeBlock();
		if(block_index == -1){
			printf("error: no free blocks\n");
			//TODO: maybe clean up stuff here
			return;
		}
		used_blocks[block_index] = 1;
		
		//get next free index in inode block array, error check & set entry to current block index
		int inode_block_entry = findFreeInodeBlockEntry(inode_idx);
		if(inode_block_entry == -1) {
			printf("error: no free node blocks\n");
			//TODO: maybe clean up stuff here
			return;
		}
		inode_array_ptr[inode_idx]->blocks[inode_block_entry] = block_index;
		
		//move forward in the file and read data into current block
		fseek(ifp, offset, SEEK_SET);
		int bytes = fread(file_data[block_index], BLOCK_SIZE, 1, ifp);
		
		// error if no bytes read while still in file
		if( bytes == 0 && !feof( ifp ) ) {
			printf("An error occured reading from the input file.\n");
			return;
		}
		
		//gets rid of some dumb error
		clearerr(ifp);
		
		//increment copy size and offset for next loop
		copy_size -= BLOCK_SIZE;
		offset += BLOCK_SIZE;
	}
	
	//copies in last bit of data if less than a block left
	if (copy_size > 0) {
		//I wont explain all this because its like exactly the same as above,
		//just not in a loop
		int block_index = findFreeBlock();
		if(block_index == -1){
			printf("error: no free blocks\n");
			//TODO: maybe clean up stuff here
			return;
		}
		
		used_blocks[block_index] = 1;
		int inode_block_entry = findFreeInodeBlockEntry(inode_idx);
		if(inode_block_entry == -1) {
			printf("error: no free node blocks\n");
			//TODO: maybe clean up stuff here
			return;
		}
		inode_array_ptr[inode_idx]->blocks[inode_block_entry] = block_index;
		
		fseek(ifp, offset, SEEK_SET);
		int bytes = fread(file_data[block_index], copy_size, 1, ifp);
	}
	
	//done reading from file, can be closed
	fclose(ifp);
	
	printf("put file %s in %s\n", filename, open_fs);
	
	return;
}

//takes file in filesystem with name infile, and writes to external file with name outfile
void get(char * infile, char * outfile) {
	//gets index of inode with name of infile
	int i;
	int infile_index = -1;
	for(i = 0; i < MAX_NUM_FILES; i++) {
		if(strcmp(directory_ptr[i].name, infile) == 0) {
			infile_index = directory_ptr[i].inode_idx;
			break;
		}
	}
	//error if name was not found
	if(infile_index == -1) {
		printf("could not find file: %s\n", infile);
	}
	
	//open file for writing to
	FILE *ofp;
	ofp = fopen(outfile, "w");
	
	//error checking
	if( ofp == NULL ) {
		printf("Could not open output file: %s\n", outfile );
		perror("Opening output file returned");
		return;
	}
	
	//set values for the copy loop
	int inode_block_index = 0;
	int offset = 0;
	int copy_size = inode_array_ptr[infile_index]->size;
	int block_index;
	
	//write out data in blocks
	while( copy_size > 0 ) { 
		
		//set amount of bytes for writing out to file
		//will be full block sizes until last block which may not be full
		int num_bytes;
		if( copy_size < BLOCK_SIZE ) {
			num_bytes = copy_size;
		}
		else {
			num_bytes = BLOCK_SIZE;
		}
		
		//gets index of current block and writes it to file
		block_index = inode_array_ptr[infile_index]->blocks[inode_block_index++];
		fwrite( file_data[block_index], num_bytes, 1, ofp ); 
		
		//sets values for next loop
		copy_size -= BLOCK_SIZE;
		offset    += BLOCK_SIZE;
		block_index ++;
		
		//moves file forward for next write
		fseek( ofp, offset, SEEK_SET );
	}

	// Close the output file, we're done. 
	fclose( ofp );
	
	printf("retrieved %s from %s as %s", infile, open_fs, outfile);
}

//writes out filesystem data to its own file
//just a weird version of get that writes out all the blocks instead of specific ones
void savefs(char * filename) {
	//open file for writing and error check
	FILE *ofp;
	ofp = fopen(filename, "w");

	if( ofp == NULL ) {
		printf("Could not open output file: %s\n", filename );
		perror("Opening output file returned");
		return;
	}
	
	//set values for looping
	int block_index = 0;
	int offset = 0;
	int copy_size = NUM_BLOCKS*BLOCK_SIZE;
	
	//copy out all of file_data in block-sized chunks
	while( copy_size > 0 ) { 
		//this should always be full blocks, but good to be safe
		int num_bytes;
		if( copy_size < BLOCK_SIZE ) {
			num_bytes = copy_size;
		}
		else {
			num_bytes = BLOCK_SIZE;
		}
		
		//write out the current block
		fwrite( file_data[block_index], num_bytes, 1, ofp ); 
		
		//set values for next loop
		copy_size -= BLOCK_SIZE;
		offset    += BLOCK_SIZE;
		block_index ++;
		
		//move file forward
		fseek( ofp, offset, SEEK_SET );
	}

	// Close the output file, we're done. 
	fclose( ofp );
}

//verifies file is within max length and only contains alphanumeric chars and '.'
int checkFileName(char * filename) {
	if(strlen(filename) > MAX_FILENAME) {
		return -1;
	}
	
	int i;
	for(i = 0; i < strlen(filename); i++) {
		if(!isalnum(filename[i]) && filename[i] != '.') {
			return -1;
		}
	}
	return 1;
}

int main() {
	init();
/*
 *
 * ------------------------------------SHELL CODE--------------------------------------------------
 *
 */
	char * command_string = (char*) malloc( MAX_COMMAND_SIZE );
	
	// everyone's favorite for loop iterator!
	int i;
	
	// used as a boolean to check whether or not a command is being recycled,
	// or if the shell should grab a new one from stdin
	int reuse_cmd = 0;

	while( 1 ) {
		if( !reuse_cmd ) {
			// Print out the mfs prompt
			//printf("current filesystem: %s\n", open_fs);
			printf ("mfs> ");

			// Read the command from the commandline.  The
			// maximum command that will be read is MAX_COMMAND_SIZE
			// This while command will wait here until the user
			// inputs something since fgets returns NULL when there
			// is no input
			while( !fgets (command_string, MAX_COMMAND_SIZE, stdin) );
		}
		// set the reuse cmd back to false so the shell will get input from user next time
		else {
			reuse_cmd = 0;
		}

		// Parse input 
		char *token[MAX_NUM_ARGUMENTS];

		int   token_count = 0;                                 
		
		// Pointer to point to the token
		// parsed by strsep
		char *argument_ptr;                                         
		
		char *working_string = strdup( command_string );                

		// we are going to move the working_string pointer so
		// keep track of its original value so we can deallocate
		// the correct amount at the end
		char *head_ptr = working_string;

		// Tokenize the input strings with whitespace used as the delimiter
		while ( ( (argument_ptr = strsep(&working_string, WHITESPACE ) ) != NULL) && 
				(token_count<MAX_NUM_ARGUMENTS)) {
			token[token_count] = strndup( argument_ptr, MAX_COMMAND_SIZE );
			if( strlen( token[token_count] ) == 0 ) {
				token[token_count] = NULL;
			}
			token_count++;
		}

		// due to the possibility of a continue, figured it'd be better to free this here 
		// since it's not used further down anyway
		free( head_ptr );
		
		// checks for blank line, and if so restarts the loop to get more input
		if( token[0] == NULL ) {
			continue;
		}
/*
 *
 * ------------------------------------FILE COMMANDS-----------------------------------------------
 *
 */
		
		//*****************************************************************************************
		// put <filename>
		// Copy the local file to the filesystem image
		
		else if( strcmp( token[0], "put" ) == 0  && token_count == 3) {
			//verify file name
			int status = checkFileName(token[1]);
			if(status == -1) {
				printf("error: file name too long or not alphanumeric\n");
				continue;
			}
			
			//all the hard stuff already done above
			put(token[1]);
			continue;
		}
		
		//*****************************************************************************************
		// get <filename> <newfilename>
		// Retrieve the file form the file system image 
		// and place it in the file named <newfilename> if specified
	
		else if( strcmp( token[0], "get" ) == 0 && (token_count == 3 || token_count == 4)) {
			//calls get function with either two different file names or with the same names
			//depending on number of tokens
			if(token_count == 4) {
				get(token[1], token[2]);
			}
			else {
				get(token[1], token[1]);
			}
			continue;
		}
		
		//*****************************************************************************************
		// del <filename>
		// Delete the file
		else if( strcmp( token[0], "del" ) == 0 && token_count == 3) {
			//finds index of file for deleting
			int i;
			int infile_index = -1;
			
			for(i = 0; i < MAX_NUM_FILES; i++) {
				if(strcmp(directory_ptr[i].name, token[1]) == 0) {
					infile_index = i;
					break;
				}
			}
			
			//checks if file was not found
			if(infile_index == -1) {
				printf("could not find file: %s\n", token[1]);
				continue;
			}
			
			//cant delete if read-only
			if(directory_ptr[infile_index].r == 1) {
				printf("error: requested file is read-only and cannot be deleted\n");
				continue;
			}
			
			//sets the directory, inode, and blocks to be free
			directory_ptr[infile_index].valid = 0;
			int inode_idx = directory_ptr[infile_index].inode_idx;
			inode_array_ptr[inode_idx]->valid = 0;
			int block_index;
			for(i = 0; i < MAX_BLOCKS_PER_FILE; i++) {
				if(inode_array_ptr[inode_idx]->blocks[i] != -1) {
					block_index = inode_array_ptr[inode_idx]->blocks[i];
					del_blocks[i] = block_index;
					inode_array_ptr[inode_idx]->blocks[i] = -1;
					used_blocks[block_index] = 0;
				}
			}
			
			printf("deleted file %s\n", token[1]);
			
			continue;
		}
		
		//*****************************************************************************************
		// undel <filename>
		// Undelete the file if found in the directory
		else if( strcmp( token[0], "undel" ) == 0 && token_count == 3) {
			//finds index of file for undeleting
			int i;
			int infile_index = -1;
			
			for(i = 0; i < MAX_NUM_FILES; i++) {
				if(strcmp(directory_ptr[i].name, token[1]) == 0) {
					infile_index = i;
					break;
				}
			}
			
			//checks if file was not found
			if(infile_index == -1) {
				printf("could not find file: %s\n", token[1]);
				continue;
			}
			
			//sets the directory, inode, and blocks to be free
			directory_ptr[infile_index].valid = 1;
			int inode_idx = directory_ptr[infile_index].inode_idx;
			inode_array_ptr[inode_idx]->valid = 1;
			
			//repeats the process from put, since the file should be just deleted it,
			//it should find the same blocks it previously had
			int block_index;
			for(i = 0; i < MAX_BLOCKS_PER_FILE; i++) {
				if(del_blocks[i] != -1) {
					//get block for storing, error check and set to be in use
					block_index = del_blocks[i];
					used_blocks[block_index] = 1;
					
					//get next free index in inode block array, error check & set entry to current block index
					int inode_block_entry = findFreeInodeBlockEntry(inode_idx);
					if(inode_block_entry == -1) {
						printf("error: no free node blocks\n");
						//TODO: maybe clean up stuff here
						continue;
					}
					inode_array_ptr[inode_idx]->blocks[inode_block_entry] = block_index;
				}
			}
			
			printf("undeleted file %s\n", token[1]);
			continue;
		}
		
		//*****************************************************************************************
		// list [-h] 
		// List the files in the file system image
		else if( strcmp( token[0], "list" ) == 0 && token_count <= 3) {
			//flag for whether or not any files are found
			int no_files = 1;
			//loops through each entry in directory and checks if valid
			for(i = 0; i < MAX_NUM_FILES; i++) {
				//only prints the entry if not hidden, or if -h tag was included
				if(directory_ptr[i].valid == 1 && 
					(directory_ptr[i].h == 0 || 
					(token_count == 3 && strcmp(token[1], "-h") == 0))) {
					//file was found, set flag
					no_files = 0;
					
					//get time values for printing
					int inode_idx = directory_ptr[i].inode_idx;
					time_t date = inode_array_ptr[inode_idx]->date;
					struct tm *t = localtime(&date);
					
					int hours = t->tm_hour;
					int minutes = t->tm_min;    
				 
					int day = t->tm_mday;
					int month = t->tm_mon + 1; 
					
					//print all the info on current entry
					printf("%d %02d/%02d %02d:%02d %s\n", directory_ptr[i].inode_idx, month, day, 
															hours, minutes, directory_ptr[i].name);
				}
			}
			
			//if no file was found, no_files will be 1 instead of 0
			if(no_files) {
				printf("no files found\n");
			}
			continue;
		}
		
		//*****************************************************************************************
		// df
		// Display the amount of disk space left in the file system
		else if( strcmp( token[0], "df" ) == 0 && token_count == 2) {
			//this abstraction feels so nice compared to whatever i had before watching the
			//thanksgiving video
			printf("free disk space: %d\n", df());
			continue;
		}
		
		//*****************************************************************************************
		// open <file image name> 
		// Open a file system image
		else if( strcmp( token[0], "open" ) == 0 && token_count == 3) {
			//this is just a weird version of put
			
			//get file size and open file
			struct stat buf;  
			int status =  stat( token[1], &buf );
			FILE *ifp = fopen ( token[1], "r" ); 
			
			//set values for copy loop
			int copy_size = buf . st_size;
			int offset = 0;               
			int block_index = 0;
			
		//copy file into file_data in block sized chunks
			while( copy_size > 0 )
			{
				//move file forward
				fseek( ifp, offset, SEEK_SET );
				
				//write in current block
				int bytes  = fread( file_data[block_index], BLOCK_SIZE, 1, ifp );
				
				//error check
				if( bytes == 0 && !feof( ifp ) )
				{
					printf("An error occured reading from the input file.\n");
					return -1;
				}
				
				//get rid of weird error
				clearerr( ifp );
				
				//set values for next loop
				copy_size -= BLOCK_SIZE;
				offset    += BLOCK_SIZE;
				block_index ++;
			}

			// We are done copying from the input file so close it out.
			fclose( ifp );
			
			//set name for current open filesystem
			strcpy(open_fs, token[1]);
			
			printf("opened filesystem %s\n", open_fs);
			continue;
		}
		
		//*****************************************************************************************
		// close
		// Close the currently opened file system
		else if( strcmp( token[0], "close" ) == 0 && token_count == 2) {
			//call initialize function to reset all values
			init();
			printf("closed filesystem %s\n", open_fs);
			//erase current filesystem name
			strcpy(open_fs, "");
			continue;
		}
		
		//*****************************************************************************************
		// createfs < disk image name >
		// Create a new file system image
		else if( strcmp( token[0], "createfs" ) == 0 && token_count == 3) {
			//verify filename
			int status = checkFileName(token[1]);
			if(status == -1) {
				printf("error: file name too long or not alphanumeric\n");
				continue;
			}
			
			//erases current file data to write out fresh filesystem to a file
			//i could probably save the current data using a temp file
			//but im way too lazy for that
			init();
			savefs(token[1]);
			printf("created new filesystem %s\n", token[1]);
			
			continue;
		}
		
		//*****************************************************************************************
		// savefs
		// Save the current file system image
		else if( strcmp( token[0], "savefs" ) == 0 && token_count == 2) {
			//calls function defined above
			savefs(open_fs);
			printf("saved filesystem %s\n", open_fs);
			
			continue;
		}
		
		//*****************************************************************************************
		// attrib [+attribute] [ -attribute] <filename> 
		// Set or remove the attribute for the file
		else if( strcmp( token[0], "attrib" ) == 0 && token_count == 4) {
			//gets index of file for adding attribute to
			int i;
			int infile_index = -1;
			for(i = 0; i < MAX_NUM_FILES; i++) {
				if(strcmp(directory_ptr[i].name, token[2]) == 0) {
					infile_index = i;
					break;
				}
			}
			
			//if -1, file was not found
			if(infile_index == -1) {
				printf("could not find file: %s\n", token[2]);
				continue;
			}
			
			//sets attribute depending on first 2 chars of token 2
			if(token[1][0] == '+') {
				if(token[1][1] == 'h') {
					directory_ptr[infile_index].h = 1;
				}
				else if(token[1][1] == 'r') {
					directory_ptr[infile_index].r = 1;
				}
			}
			else if(token[1][0] == '-') {
				if(token[1][1] == 'h') {
					directory_ptr[infile_index].h = 0;
				}
				else if(token[1][1] == 'r') {
					directory_ptr[infile_index].r = 0;
				}
			}
			
			printf("attributed file %s with %s\n", token[2], token[1]);
			
			continue;
		}
		
		//*****************************************************************************************
		// quit (or exit)
		// exits the program if user types either of the magic words
		else if( strcmp( token[0], "quit" ) == 0 || strcmp( token[0], "exit" ) == 0 ) {
			return 0;
		}
		
		//*****************************************************************************************
		// any other input to the shell is an unknown command,
		else {
			printf("%s: command not found or incorrect number of parameters\n", token[0]);
		}	
	}
	return 0;
}

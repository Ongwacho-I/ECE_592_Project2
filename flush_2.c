#define _GNU_SOURCE                         // Enable Linux CPU-affinity functions.
#include <stdio.h>                          // Printing and error messages.
#include <stdlib.h>                         // Number conversion and sorting.
#include <stdint.h>                         // uint64_t.
#include <inttypes.h>                       // Portable uint64_t print formats.
#include <errno.h>                          // Number-conversion errors.
#include <fcntl.h>                          // open and O_RDONLY.
#include <unistd.h>                         // close and sysconf.
#include <sys/stat.h>                       // File size and type checks.
#include <sys/mman.h>                       // mmap and munmap.
#include <sched.h>                          // CPU affinity and sched_yield.
#include <time.h>                           // Monotonic elapsed time.
#include <cpuid.h>                          // CPU feature checks.
#include <x86intrin.h>                      // RDTSC, CLFLUSH and fences.

/* Linux x86-64, GCC/Clang. Compile on each machine:
   gcc -O0 -Wall -Wextra flush_flush_simple.c -o flush_flush
   ./flush_flush TARGET_FILE HEX_OFFSET [PROBES=10000] [PRINT_EVERY=100]
   Example: ./flush_flush sample.bin 40 10000 1
   The file must already exist; offset 40 means 64 bytes, not decimal 40.
   The file is the TARGET, not the output. Do not truncate it while running.
   Calibration reads the target; monitoring only flushes it.
   Timing includes fence/timer overhead. TSC ticks are not core cycles.
   Fences do not prevent interrupts, preemption or speculative cache refills.
   Intended for modern lab x86-64 CPUs; no guaranteed separation on every CPU.
   Local cached calibration does not prove accurate cross-core detection.
   Instruction reference: Intel SDM, CLFLUSH, RDTSC, LFENCE and MFENCE.
*/

#define N 1000                              // Samples per calibration condition.

uint64_t get_number(char *text, int base) {   // Parse a command-line number.
    char *end;                              // Points to unconverted text.
    errno = 0;                              // Clear any old conversion error.
    uint64_t n = strtoull(text, &end, base);  // Convert using base 10 or 16.
    if (errno || !*text || *text == '-' || *end) { // Reject invalid input.
        fprintf(stderr, "Invalid number: %s\n", text); // Show the bad argument.
        exit(1);                            // Stop on invalid input.
    }                                       // End input check.
    return n;                               // Return the parsed number.
}                                           // End get_number.

uint64_t flushandflush(const void *addr) {    // Time a flush, without loading addr.
    _mm_mfence();                           // Order earlier memory operations.
    _mm_lfence();                           // Order execution before the timer.
    uint64_t start = __rdtsc();              // Read starting TSC ticks.
    _mm_lfence();                           // Keep the flush after the timestamp.
    _mm_clflush(addr);                      // Flush the target cache line.
    _mm_mfence();                           // Order the flush before ending timing.
    _mm_lfence();                           // Execution boundary before RDTSC.
    uint64_t end = __rdtsc();                // Read ending TSC ticks.
    _mm_lfence();                           // Keep later work after the timestamp.
    return end - start;                     // Includes measurement overhead.
}                                           // End flushandflush.

uint64_t sample(unsigned char *addr, int cached) { // Prepare a calibration sample.
    _mm_clflush(addr);                      // Start with a flushed target line.
    _mm_mfence();                           // Order the flush before preparation.
    if (cached) {                           // Prepare cached data only when asked.
        _mm_lfence();                       // Execution boundary before the load.
        (void)*(volatile unsigned char *)addr; // Force a read of the target byte.
        _mm_mfence();                       // Order the load before measurement.
    }                                       // End cached-data preparation.
    return flushandflush(addr);             // Use the same timer as monitoring.
}                                           // End sample.

int compare(const void *a, const void *b) {   // Comparator for sorting timings.
    uint64_t x = *(const uint64_t *)a;       // Read the first value.
    uint64_t y = *(const uint64_t *)b;       // Read the second value.
    return (x > y) - (x < y);                // Return 1, 0 or -1.
}                                           // End compare.

double seconds(void) {                      // Read a clock independent of TSC Hz.
    struct timespec t;                      // Store seconds and nanoseconds.
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { // Detect clock failure.
        perror("clock_gettime"); exit(1);   // Report the failure and stop.
    }                                       // End clock check.
    return t.tv_sec + t.tv_nsec / 1e9;       // Convert to seconds.
}                                           // End seconds.



int main(int argc, char **argv) {            // Main program.
    setvbuf(stdout, NULL, _IOLBF, 0);        // Print complete lines promptly.
    if (argc < 3 || argc > 5) {              // Require file and hexadecimal offset.
        fprintf(stderr, "Usage: %s FILE HEX_OFFSET [PROBES] [PRINT_EVERY]\n", argv[0]); // Show usage.
        return 1;                           // Stop if arguments are missing.
    }                                       // End argument-count check.


    uint64_t offset = get_number(argv[2], 16); // Offset remains hexadecimal.
    uint64_t count = argc > 3 ? get_number(argv[3], 10) : 10000; // Default 10000 probes.
    uint64_t every = argc > 4 ? get_number(argv[4], 10) : 100; // Default print interval.
    if (!count || count > 100000000 || !every) { // Keep counts valid and bounded.
        puts("Use 1..100000000 probes and a positive print interval."); return 1; // Report error.
    }                                       // End count check.

    unsigned int a, b, c, d;                 // Storage for CPU feature registers.


    if (!__get_cpuid(1, &a, &b, &c, &d) || !(d & (1u << 19)) || !(d & (1u << 4))) { // Check CLFLUSH/TSC.
        puts("CPU must support CLFLUSH and TSC."); return 1; // Stop on unsupported hardware.
    }                                       // End feature check.


    cpu_set_t allowed, chosen;              // Allowed CPUs and selected CPU.
    if (sched_getaffinity(0, sizeof allowed, &allowed)) { perror("affinity"); return 1; } // Read allowed CPUs.
    int cpu = 0;                            // Start looking at logical CPU zero.


    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed)) ++cpu; // Find first permitted CPU.
    if (cpu == CPU_SETSIZE) { puts("No allowed CPU."); return 1; } // Reject an empty set.


    CPU_ZERO(&chosen); CPU_SET(cpu, &chosen); // Select only that CPU, respecting taskset.

    if (sched_setaffinity(0, sizeof chosen, &chosen)) { perror("pin CPU"); return 1; } // Prevent migration.

    int fd = open(argv[1], O_RDONLY);        // Open the existing target file.

    if (fd < 0) { perror("open"); return 1; } // Report failure to open it.

    struct stat st;                         // Store file metadata.

    if (fstat(fd, &st)) { perror("fstat"); close(fd); return 1; } // Read file size/type.

    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || offset >= (uint64_t)st.st_size) { // Validate target.
        puts("File must be nonempty and offset must be inside it."); close(fd); return 1; // Reject invalid file.
    }                                       // End file validation.


    long page = sysconf(_SC_PAGESIZE);       // Read the operating-system page size.

    if (page <= 0) { puts("Invalid page size."); close(fd); return 1; } // Check page size.

    size_t within = offset % (uint64_t)page; // Position of target inside its page.

    unsigned char *map = mmap(NULL, within + 1, PROT_READ, MAP_SHARED, fd, (off_t)(offset - within)); // Map target page.
    close(fd);                              // The mapping keeps its own file reference.

    if (map == MAP_FAILED) { perror("mmap"); return 1; } // Check mapping success.

    unsigned char *addr = map + within;     // Address of the requested target byte.

    uint64_t hot[N], cold[N];                // Cached and flushed calibration timings.

    printf("CPU %d | target %s + 0x%" PRIx64 "\nCalibrating...\n", cpu, argv[1], offset); // Show selected target.

    for (int i = 0; i < 200; ++i) (void)sample(addr, i & 1); // Warm up both conditions.
    
    for (int i = 0; i < N; ++i) {            // Collect calibration sample pairs.
        if (i & 1) { cold[i] = sample(addr, 0); hot[i] = sample(addr, 1); } // Flushed first on odd iterations.
        else { hot[i] = sample(addr, 1); cold[i] = sample(addr, 0); } // Cached first on even iterations.
    }                                       // End calibration collection.


    qsort(hot, N, sizeof hot[0], compare);  // Sort cached timings.

    qsort(cold, N, sizeof cold[0], compare); // Sort flushed timings.

    uint64_t h = hot[N / 2], m = cold[N / 2]; // Take middle sorted values as medians.
    
    uint64_t threshold = h < m ? h + (m - h) / 2 : m + (h - m) / 2; // Midpoint threshold.
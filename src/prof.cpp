#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <map>
#include <vector>
#include <string>

#include "common.h"
#include "LoopCallProfile.h"

#define END_OF_ROW -1

// sample every 100 us
#define SAMPLING_INTERVAL 100

static uint32_t _prof_num_loops_tot = 0;

void _prof_init() __attribute__((constructor));
void _prof_dump() __attribute__((destructor));

// Profile data about a single loop
struct loop_data {
  char *func;
  int32_t header_id; // > 0 if it's loop, = 0 if it's a function
  int64_t runs;
};

static LoopCallProfile profile;

// Create a linked list of descriptors, one per linked module.
//
typedef struct module_desc_t {
  char *_moduleName;
  uint32_t _prof_num_loops;
  struct loop_data *_prof_loops_p;
  uint32_t *_prof_loops_running_p;
  struct module_desc_t *next;
} module_desc;

module_desc *module_desc_list_head = NULL;
module_desc *module_desc_list_tail = NULL;

static module_desc *get_new_desc_list_node() {
  module_desc *new_entry = new module_desc();
  if (new_entry == NULL) {
    perror("append_new_desc_list_node(): ");
    exit(1);
  }
  if (module_desc_list_tail == NULL)
    module_desc_list_head = new_entry;
  else {
    assert(module_desc_list_tail->next == NULL && "Incorrect list tail");
    module_desc_list_tail->next = new_entry;
  }
  module_desc_list_tail = new_entry;
  return new_entry;
}

extern "C" void add_module_desc(char* moduleName,
				int32_t *_numloops, struct loop_data *_p_l,
                                int32_t *_p_l_r) {
  int32_t numloops = *_numloops;
  assert(numloops >= 0 && "Unexpected negative value for _numloops");
  _prof_num_loops_tot += (uint32_t)numloops;

  module_desc *new_entry = get_new_desc_list_node();
  new_entry->_moduleName = strdup(moduleName);
  new_entry->_prof_num_loops = (uint32_t)numloops;
  new_entry->_prof_loops_p = _p_l;
  new_entry->_prof_loops_running_p = (uint32_t *)_p_l_r;
  new_entry->next = NULL;
#ifndef NDEBUG
  printf("Registering one module desc!\n");
#endif
}

static struct timespec begin;

FILE *dumpfile;
size_t dumpsize;
size_t num_sampled;

// exponential distribution with lambda = 1
// note that this means the expected value is also one (E[X] = 1/lambda)
//
// stolen from:
// https://en.wikipedia.org/wiki/Exponential_distribution#Generating_exponential_variates
static inline float rand_exp() {
  float r = (float)rand() / (float)RAND_MAX, x = -logf(r);
  return x;
}

static void dump_sample(int signo);

// setup a timer that fires in T microseconds
// T is a exponential random variable with expectation of `SAMPLING_INTERVAL`
// microseconds
static void setup_timer() {
  struct itimerval timerspec;

  timerspec.it_interval.tv_sec = 0;
  timerspec.it_interval.tv_usec = 0;
  timerspec.it_value.tv_sec = 0;
  timerspec.it_value.tv_usec = SAMPLING_INTERVAL * (rand_exp() + 0.5);

  if (signal(SIGPROF, dump_sample) == SIG_ERR) {
    perror("Unable to catch SIGPROF");
    exit(1);
  }

  setitimer(ITIMER_PROF, &timerspec, NULL);
}

// running_instance[] is an array of integers recording which loops were
// running at the time of the sample.  Use those to accumulate an NxN
// matrix of profile entries, where profile[i][j] is for loops i and j,
// and each profile entry is as defined above.
//
static void collect_sample_impl(
    std::vector<std::pair<unsigned, unsigned>> running_instance) {
  for (unsigned i = 0, e = running_instance.size(); i != e; i++) {
    const auto &coli = running_instance[i];
    profile.getFreq(coli.first, coli.first) += 1;

    for (unsigned j = i + 1, e = running_instance.size(); j != e; j++) {
      const auto &colj = running_instance[j];
      assert(coli.first < colj.first);

      if (coli.second < colj.second) {
        // i called j
        profile.getFreq(coli.first, colj.first) += 1;
      } else {
        // j called i
        profile.getFreq(colj.first, coli.first) += 1;
      }
    }
  }
  printf("\n");
}

// turn [(col1, val1), ...] into [val1, val2, ...]
// return bytes read from `dump`
static int uncompress_one_row(std::vector<std::pair<unsigned, unsigned>> &row,
                              int32_t *dump) {
  int i = 0;
  for (;;) {
    int32_t col = dump[i++];
    if (col == END_OF_ROW)
      break;

    uint32_t val = dump[i++];
    row.push_back({col, val});
  }
  return i;
}

static void collect_samples(int32_t *dump) {
  // Nothing to do if there are zero loops
  if (_prof_num_loops_tot == 0)
    return;

  std::vector<std::pair<unsigned, unsigned>> running_instance;

  size_t i;
  for (i = 0; i < num_sampled; i++) {

#ifndef NDEBUG
    // Debug infinite loop
    static int inCollect = 0;
    printf("In collect_sample %d\n", ++inCollect);
#endif

    // update profile of all the loops given a new sample
    dump += uncompress_one_row(running_instance, dump);
    collect_sample_impl(running_instance);
    running_instance.resize(0);
  }
}

// similar to `fwrite` except guaranteeing writing all bytes
static int fwriteall(void *buf, size_t size, FILE *f) {
  size_t num_written = 0;
  do {
    ssize_t bytes = fwrite((char *)buf + num_written, 1, size - num_written, f);
    assert(bytes >= 0 && "fwrite failed");
    num_written += bytes;
  } while (num_written < size);
  return 0;
}

// Sample what loops and functions are running and write sample data to a file
static void dump_one_sample(module_desc *desc, uint32_t global_idx) {
  // Write the sample data for each module
  uint32_t i;
  for (i = 0; i < desc->_prof_num_loops; i++, global_idx++) {
    if (desc->_prof_loops_running_p[i] == 0)
      continue;

    fwriteall(&global_idx, sizeof(uint32_t), dumpfile);
    fwriteall(desc->_prof_loops_p + i, sizeof(uint32_t), dumpfile);
    dumpsize += sizeof(uint32_t) * 2;
  }
}

// Sample what loops and functions are running and write sample data to a file
static void dump_sample(int signo) {
  uint32_t global_idx = 0;
  for (module_desc *desc = module_desc_list_head; desc != NULL;
       desc = desc->next) {
    dump_one_sample(desc, global_idx);
    global_idx += desc->_prof_num_loops;
  }

  // write a token to signify end of the row
  int end = END_OF_ROW;
  fwriteall(&end, sizeof(uint32_t), dumpfile);
  dumpsize += sizeof(uint32_t);

  num_sampled++;
  setup_timer();
}

void _prof_init() {
  dumpfile = fopen(ProfileDumpFileName, "w+");

  srand(time(NULL));
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin);
  setup_timer();
}

void _prof_dump() {
  // disarm timer
  struct itimerval timerspec;
  memset(&timerspec, 0, sizeof timerspec);
  setitimer(SIGPROF, &timerspec, NULL);
  signal(SIGPROF, SIG_IGN);

  // Time the process in ms. For accuracy, do this before postprocessing.
  struct timespec end;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
  long long int elapsed =
      (end.tv_sec - begin.tv_sec) * 1e3 + (end.tv_nsec - begin.tv_nsec) / 1e6;

  // read sample from the dump
  fflush(dumpfile);
  rewind(dumpfile);
  int32_t *dump = (int32_t *)mmap(NULL, dumpsize, PROT_READ, MAP_PRIVATE,
                                  fileno(dumpfile), 0);
  assert(dump && "failed to mmap dumpfile");
  fclose(dumpfile);
  collect_samples(dump);

  FILE *flat_out = fopen(MetadataFileName, "wb");

  fprintf(flat_out, "module,function,header-id,runs,time(pct),time(ms)\n");
  uint32_t loop_idx = 0;
  for (module_desc *desc = module_desc_list_head; desc != NULL;
       desc = desc->next) {
    struct loop_data *prof_loops = desc->_prof_loops_p;
    for (uint32_t i = 0; i < desc->_prof_num_loops; i++) {
      struct loop_data *loop = &prof_loops[i];
      float pct = (float)profile.getFreq(loop_idx, loop_idx) / num_sampled;
      assert(pct <= 1.0);
      fprintf(flat_out, "%s,%s,%d,%ld,%.4f,%.4f\n",
	      desc->_moduleName, loop->func, loop->header_id,
              loop->runs, 100 * pct, elapsed * pct);
      ++loop_idx;
    }
  }

  profile.dump(ProfileFileName);

  fclose(flat_out);
}

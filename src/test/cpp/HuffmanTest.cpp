#include "verilated.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>


// <editor-fold> ugly pre-processor macros
#define _STR(s) #s
#define STR(s) _STR(s)
#define _CAT(s,t) s##t
#define CAT(s,t) _CAT(s,t)
// </editor-fold>

#ifndef COMPRESSOR
#define COMPRESSOR HuffmanCompressor
#endif
#ifndef DECOMPRESSOR
#define DECOMPRESSOR HuffmanDecompressor
#endif
#define VCOMPRESSOR CAT(V,COMPRESSOR)
#define VDECOMPRESSOR CAT(V,DECOMPRESSOR)
#include STR(VCOMPRESSOR.h)
#include STR(VDECOMPRESSOR.h)


#ifndef TRACE_ENABLE
#define TRACE_ENABLE false
#endif
#if TRACE_ENABLE
  #include "verilated_vcd_c.h"
  #define COMPRESSOR_TRACE() do { \
    if(compressorTraceEnable) { \
      compressorTrace->dump(compressorContext::time()); \
      compressorContext::timeInc(1); \
    } while(false)
  #define DECOMPRESSOR_TRACE() do { \
    if(decompressorTraceEnable) { \
      decompressorTrace->dump(decompressorContext::time()); \
      decompressorContext::timeInc(1); \
    } while(false)
#else
  #define COMPRESSOR_TRACE() do {} while(false)
  #define DECOMPRESSOR_TRACE() do {} while(false)
#endif


#ifndef TIMEOUT
#define TIMEOUT (idle >= 1000)
#endif


#define ARG_DUMP_FILENAME 1
#define ARG_REPORT_FILENAME 2
#define ARG_COMPRESSOR_TRACE 3
#define ARG_DECOMPRESSOR_TRACE 4

#define PAGE_SIZE 4096

#define STAGE_LOAD 0
#define STAGE_COMPRESSOR 1
#define STAGE_DECOMPRESSOR 2
#define STAGE_FINALIZE 3
#define NUM_STAGES 4

#define JOB_QUEUE_SIZE 10


static size_t min(size_t a, size_t b) {return a <= b ? a : b;}
static size_t max(size_t a, size_t b) {return a >= b ? a : b;}


struct Job {
  int stage;
  int id;
  
  uint8_t *raw;
  size_t rawLen;
  size_t rawCap;
  uint8_t *compressed;
  size_t compressedLen; // in bits, not bytes
  size_t compressedCap; // in bytes
  uint8_t *decompressed;
  size_t decompressedLen;
  size_t decompressedCap;
  
  int compressorCycles;
  int decompressorCycles;
  int compressorStallCycles;
  int decompressorStallCycles;
};
struct Summary {
  size_t totalSize;
  int totalPages;
  size_t nonzeroSize;
  int nonzeroPages;
  size_t processedSize;
  size_t processedPages;
  
  int passedPages;
  int failedPages;
  
  int compressorCycles;
  int compressorStalls;
  int decompressorCycles;
  int decompressorStalls;
};

static VCOMPRESSOR *compressor;
static VDECOMPRESSOR *decompressor;
#if TRACE_ENABLE
static VerilatedVcdC *compressorTrace;
static VerilatedVcdC *decompressorTrace;
static VerilatedContext compressorContext;
static VerilatedContext decompressorContext;
static bool compressorTraceEnable;
static bool decompressorTraceEnable;
#endif
static FILE *dumpfile;
static FILE *reportfile;
static Job jobs[JOB_QUEUE_SIZE];
static Summary summary;

static void doLoad();
static void doCompressor();
static void doDecompressor();
static void doFinalize();

int main(int argc, char **argv, char **env) {
  Verilated::commandArgs(argc, argv);
  compressor = new VCOMPRESSOR;
  decompressor = new VDECOMPRESSOR;
  
  #if TRACE_ENABLE
  compressorTraceEnable = argc > ARG_COMPRESSOR_TRACE &&
    (argv[ARG_COMPRESSOR_TRACE][0] != '-' ||
    argv[ARG_COMPRESSOR_TRACE][1] != '\0');
  if(compressorTraceEnable) {
    compressorContext = new VerilatedContext;
    // Verilated::traceEverOn(true);
    compressorContext::traceEverOn(true);
    compressorTrace = new VerilatedVcdC;
    compressor->trace(compressorTrace, 99);
    compressorTrace->open(argv[3]);
  }
  
  decompressorTraceEnable = argc > ARG_DECOMPRESSOR_TRACE &&
    (argv[ARG_DECOMPRESSOR_TRACE][0] != '-' ||
    argv[ARG_DECOMPRESSOR_TRACE][1] != '\0');
  if(decompressorTraceEnable) {
    decompressorContext = new VerilatedContext;
    // Verilated::traceEverOn(true);
    decompressorContext::traceEverOn(true);
    decompressorTrace = new VerilatedVcdC;
    decompressor->trace(decompressorTrace, 99);
    decompressorTrace->open(argv[ARG_DECOMPRESSOR_TRACE]);
  }
  #endif
  
  dumpfile = stdin;
  if(argc > ARG_DUMP_FILENAME && (argv[ARG_DUMP_FILENAME][0] != '-' ||
      argv[ARG_DUMP_FILENAME][1] != '\0'))
    dumpfile = fopen(argv[ARG_DUMP_FILENAME], "r");
  
  reportfile = stdout;
  if(argc > ARG_REPORT_FILENAME && (argv[ARG_REPORT_FILENAME][0] != '-' ||
      argv[ARG_REPORT_FILENAME][1] != '\0'))
    reportfile = fopen(argv[ARG_DUMP_FILENAME], "w");
  
  for(int i = 0; i < JOB_QUEUE_SIZE; i++) {
    jobs[i].stage = 0;
    jobs[i].raw = NULL;
    jobs[i].rawLen = 0;
    jobs[i].rawCap = 0;
    jobs[i].compressed = NULL;
    jobs[i].compressedLen = 0;
    jobs[i].compressedCap = 0;
    jobs[i].decompressed = NULL;
    jobs[i].decompressedLen = 0;
    jobs[i].decompressedCap = 0;
    jobs[i].compressorCycles = 0;
    jobs[i].decompressorCycles = 0;
    jobs[i].compressorStallCycles = 0;
    jobs[i].decompressorStallCycles = 0;
  }
  
  summary.totalSize = 0;
  summary.totalPages = 0;
  summary.nonzeroSize = 0;
  summary.nonzeroPages = 0;
  summary.processedSize = 0;
  summary.processedPages = 0;
  summary.passedPages = 0;
  summary.failedPages = 0;
  summary.compressorCycles = 0;
  summary.compressorStalls = 0;
  summary.decompressorCycles = 0;
  summary.decompressorStalls = 0;
  
  // assert reset on rising edge to initialize module state
  compressor->reset = 1;
  compressor->clock = 0;
  compressor->eval();
  COMPRESSOR_TRACE();
  compressor->clock = 1;
  compressor->eval();
  compressor->reset = 0;
  
  decompressor->reset = 1;
  decompressor->clock = 0;
  decompressor->eval();
  DECOMPRESSOR_TRACE();
  decompressor->clock = 1;
  decompressor->eval();
  decompressor->reset = 0;
  
  while() {
    doLoad();
    doCompressor();
    doDecompressor();
    doFinalize();
  }
  
  compressor->final();
  decompressor->final();
  
  if(dumpfile != stdin)
  fclose(dumpfile);
  
  #if TRACE_ENABLE
  if(compressorTraceEnable) {
    compressorTrace->close();
  }
  if(decompressorTraceEnable) {
    decompressorTrace->close();
  }
  #endif
  
  delete compressor;
  delete decompressor;
  
  return 0;
}

static void doLoad() {
  static int jobIdx = 0;
  struct Job *job = &jobs[jobIdx];
  if(job->stage != STAGE_LOAD)
    return;
  if(job->raw == NULL) {
    job->raw = malloc(PAGE_SIZE);
    assert(job->raw != NULL);
    job->rawCap = PAGE_SIZE;
  }
  
  size_t bytesRead =
    fread(job->raw + job->rawLen, 1, PAGE_SIZE - job->rawLen, dumpfile);
  job->rawLen += bytesRead;
  
  if(job->rawLen == PAGE_SIZE || feof(dumpfile)) {
    // finished loading page
    bool zero = true;
    for(int i = 0; i < job->rawLen; i++)
      zero = zero && job->raw[i] == 0;
    if(zero) {
      job->rawLen = 0;
    }
    else {
      summary.totalPages += 1;
      summary.totalSize += job->rawLen;
      summary.nonzeroPages += 1;
      summary.nonzeroSize += job->rawLen;
      
      job->id = summary.processedPages;
      summary.processedPages += 1;
      summary.processedSize += job->rawLen;
      
      job->stage++;
      jobIdx = ++jobIdx % JOB_QUEUE_SIZE;
    }
  }
}

static void doCompressor() {
  static int jobIdxIn = 0;
  static int jobIdxOut = 0;
  static int inBufIdx = 0;
  struct Job *jobIn = &jobs[jobIdxIn];
  struct Job *jobOut = &jobs[jobIdxOut];
  bool quit = false;
  int idle = 0;
  if(jobIn->stage != STAGE_COMPRESSOR)
    return;
  
  do {
    if(jobOut->compressedLen + COMPRESSOR_BITS_OUT > jobOut->compressedCap*8) {
      size_t *newSize = max(jobOut->compressedCap * 2, PAGE_SIZE);
      while(jobOut->compressedLen + COMPRESSOR_BITS_OUT > newSize*8)
        newSize *= 2;
      uint8_t *oldBuf = job->compressed;
      job->compressed = realloc(job->compressed, newSize);
      assert(job->compressed != oldBuf);
      job->compressedCap = newSize;
    }
    
    // expose input buffer to module
    int remaining = jobIn->rawLen - inBufIdx
    compressor->io_in_valid = min(remaining, COMPRESSOR_CHARS_IN);
    compressor->io_in_last = remaining <= COMPRESSOR_CHARS_IN;
    // module input is not in array form, so must use an ugly cast
    for(int i = 0; i < compressor->io_in_valid; i++) {
      (&compressor->io_in_data_0)[i] = jobIn->raw[inBufIdx + i];
    }
    
    compressor->io_out_ready = COMPRESSOR_BITS_OUT;
    compressor->io_out_restart = false;
    
    // update outputs based on new inputs
    compressor->eval();
    
    idle++;
    
    // shift input buffer by number of characters consumed by module input
    size_t c = min(compressor->io_in_valid, compressor->io_in_ready);
    if(c) idle = 0;
    inBufIdx += c;
    
    // push module output onto the end of output buffer
    c = min(compressor->io_out_valid, compressor->io_out_ready);
    // if(c) idle = 0;
    // module output is not in array form, so must use ugly cast
    for(int i = 0; i < c; i++) {
      int major = (jobOut->compressedLen + i) / 8;
      int minor = (jobOut->compressedLen + i) % 8;
      jobOut->compressed[major] &= (1 << minor) - 1
      jobOut->compressed[major] |= !!(&module->io_out_data_0)[i] << minor;
    }
    jobOut->compressedLen += c;
    
    compressor->io_out_restart = compressor->io_out_last &&
      compressor->io_out_ready >= compressor->io_out_valid;
    
    
    for(int i = jobIdxIn;;i = ++i % JOB_QUEUE_SIZE) {
      jobs[i].compressorCycles++;
      if(i == jobIdxOut) break;
    }
    
    if(compressor->io_in_restart) {
      jobIdxIn = ++jobIdxIn % JOB_QUEUE_SIZE;
      inBufIdx = 0;
      jobIn = &jobs[jobIdxIn];
      quit = jobIn->stage != STAGE_COMPRESSOR;
    }
    if(compressor->io_out_restart) {
      jobIdxOut = ++jobIdxOut % JOB_QUEUE_SIZE;
      jobOut->stage++;
      jobOut = &jobs[jobIdxOut];
    }
    
    summary.compressorCycles += 1;
    
    // make ure everything is still up to date
    compressor->eval();
    COMPRESSOR_TRACE();
    
    // prepare for rising edge
    compressor->clock = 0;
    compressor->eval();
    COMPRESSOR_TRACE();
    
    // update module registers with rising edge
    compressor->clock = 1;
    compressor->eval();
    
    assert(!TIMEOUT);
  } while(!quit);
}

static void doDecompressor() {
  static int jobIdxIn = 0;
  static int jobIdxOut = 0;
  static int inBufIdx = 0;
  struct Job *jobIn = &jobs[jobIdxIn];
  struct Job *jobOut = &jobs[jobIdxOut];
  bool quit = false;
  int idle = 0;
  if(jobIn->stage != STAGE_DECOMPRESSOR)
    return;
  
  do {
    if(jobOut->decompressedLen + DECOMPRESSOR_CHARS_OUT >
        jobOut->decompressedCap) {
      size_t *newSize = max(jobOut->decompressedCap * 2, PAGE_SIZE);
      while(jobOut->decompressedLen + DECOMPRESSOR_CHARS_OUT > newSize)
        newSize *= 2;
      uint8_t *oldBuf = job->decompressed;
      job->decompressed = realloc(job->decompressed, newSize);
      assert(job->compressed != oldBuf);
      job->decompressedCap = newSize;
    }
    
    // expose input buffer to module
    int remaining = jobIn->compressedLen - inBufIdx
    decompressor->io_in_valid = min(remaining, DECOMPRESSOR_BITS_IN);
    decompressor->io_in_last = remaining <= DECOMPRESSOR_BITS_IN;
    // module input is not in array form, so must use an ugly cast
    for(int i = 0; i < decompressor->io_in_valid; i++) {
      int major = (jobOut->compressedLen + i) / 8;
      int minor = (jobOut->compressedLen + i) % 8;
      (&decompressor->io_in_data_0)[i] = jobIn->compressed[major] >> minor & 1
    }
    
    decompressor->io_out_ready = DECOMPRESSOR_CHARS_OUT;
    decompressor->io_out_restart = false;
    
    // update outputs based on new inputs
    decompressor->eval();
    
    idle++;
    
    // shift input buffer by number of characters consumed by module input
    size_t c = min(decompressor->io_in_valid, decompressor->io_in_ready);
    if(c) idle = 0;
    inBufIdx += c;
    
    // push module output onto the end of output buffer
    c = min(decompressor->io_out_valid, decompressor->io_out_ready);
    // if(c) idle = 0;
    // module output is not in array form, so must use ugly cast
    for(int i = 0; i < c; i++) {
      jobOut->decompressed[jobOut->compressedLen + i] =
        (&module->io_out_data_0)[i]
    }
    jobOut->decompressedLen += c;
    
    decompressor->io_out_restart = decompressor->io_out_last &&
      decompressor->io_out_ready >= decompressor->io_out_valid;
    decompressor->eval();
    
    
    for(int i = jobIdxIn;;i = ++i % JOB_QUEUE_SIZE) {
      jobs[i].decompressorCycles++;
      if(i == jobIdxOut) break;
    }
    
    if(decompressor->io_in_restart) {
      jobIdxIn = ++jobIdxIn % JOB_QUEUE_SIZE;
      inBufIdx = 0;
      jobIn = &jobs[jobIdxIn];
      quit = jobIn->stage != STAGE_DECOMPRESSOR;
    }
    if(decompressor->io_out_restart) {
      jobIdxOut = ++jobIdxOut % JOB_QUEUE_SIZE;
      jobOut->stage++;
      jobOut = &jobs[jobIdxOut];
    }
    
    summary.decompressorCycles += 1;
    
    DECOMPRESSOR_TRACE();
    
    // prepare for rising edge
    decompressor->clock = 0;
    decompressor->eval();
    DECOMPRESSOR_TRACE();
    
    // update module registers with rising edge
    decompressor->clock = 1;
    decompressor->eval();
    
    assert(!TIMEOUT);
  } while(!quit);
}

static void doFinalize() {
  static int jobIdx = 0;
  struct Job *job = &jobs[jobIdx];
  if(job->stage != STAGE_FINALIZE)
    return;
  
  bool pass = true;
  if(job->rawLen != job->decompressedLen)
    pass = false;
  else
  for(int i = 0; i < job->rawLen; i++) {
    pass = pass && job->raw[i] == job->decompressed[i];
  }
  if(pass)
    summary.passedPages += 1;
  else
    summary.failedPages += 1;
  
  static bool printHeader = true;
  if(printHeader) {
    printHeader = false;
    fprintf(reportfile, "id,");
    fprintf(reportfile, "pass?,");
    fprintf(reportfile, "raw size,");
    fprintf(reportfile, "compressed size,");
    fprintf(reportfile, "cycles in compressor,");
    fprintf(reportfile, "cycles in decompressor,");
    fprintf(reportfile, "\n");
  }
  
  fprintf(reportfile, "%d,", job->id);
  fprintf(reportfile, "%s,", pass ? "pass" : "fail");
  fprintf(reportfile, "%lu,", job->rawLen);
  fprintf(reportfile, "%lu,", job->compressedLen);
  fprintf(reportfile, "%d,", job->compressorCycles);
  fprintf(reportfile, "%d,", job->decompressorCycles);
  fprintf(reportfile, "\n");
}

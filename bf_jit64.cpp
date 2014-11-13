// References:
// http://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-manual-325462.pdf
// http://ref.x86asm.net/
// Online assembler:
// https://defuse.ca/online-x86-assembler.htm

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <cstdint>
#include <string>

#include "bf_jit.h"

typedef void(*BrainfuckFunction)(bool (write)(void *, char c),
                                 void* write_arg,
                                 int (read)(void *),
                                 void* read_arg,
                                 void* memory);

// This is the main entry point for the implementation of "BrainfuckFunction".
// It expects it's arguments to be passed as specified in:
// http://www.x86-64.org/documentation/abi.pdf
// - 3.2  Function Calling Sequence
// - 3.2.3 Parameter Passing
const char START[] =
  // Some registers must be saved by the called function (the callee) and
  // restored on exit if they are changed. Using these registers is
  // convenient because it allows us to call the provided "write" and "read"
  // functions without worrying about our registers being changed.
  // See:
  // http://www.x86-64.org/documentation/abi.pdf "Figure 3.4: Register Usage"
  "\x41\x54"              // push   %r12  # r12 will store the "write" arg
  "\x41\x55"              // push   %r13  # r13 will store the "write_arg" arg
  "\x41\x56"              // push   %r14  # r14 will store the "read" arg
  "\x55"                  // push   %rbp  # rbp will store the "read_arg" arg
  "\x53"                  // push   %rbx  # rbx will store the "memory" arg
  
  // Store the passed arguments into a callee-saved register.
  "\x49\x89\xfc"          // mov    %rdi,%r12  # write function => r12
  "\x49\x89\xf5"          // mov    %rsi,%r13  # write arg 1 =>  r13
  "\x49\x89\xd6"          // mov    %rdx,%r14  # read function => r14
  "\x48\x89\xcd"          // mov    %rcx,%rbp  # read arg 1 => rbp
  "\x4c\x89\xc3";         // mov    %r8,%rbx   # BF memory => rbx

const char EXIT[] = 
  "\x5b"                  // pop    %rbx
  "\x5d"                  // pop    %rbp
  "\x41\x5e"              // pop    %r14
  "\x41\x5d"              // pop    %r13
  "\x41\x5c"              // pop    %r12
  "\xc3";                 // retq

// < --rbx;
const char LEFT[] = 
  "\x48\x83\xeb\x01";     // sub    $0x1,%rbx

// > ++rbx;
const char RIGHT[] =
  "\x48\x83\xc3\x01";     // add    $0x1,%rbx

// - *rbx -= 1;
const char SUBTRACT[] =
  "\x8a\x03"              // mov    (%rbx),%al
  "\x2c\x01"              // sub    $0x1,%al
  "\x88\x03";             // mov    %al,(%rbx)

// + *rbx += 1;
const char ADD[] =
  "\x8a\x03"              // mov    (%rbx),%al
  "\x04\x01"              // add    $0x1,%al
  "\x88\x03";             // mov    %al,(%rbx)

// , [part1] rax = read(rdp); if (rax == 0) goto exit; ...
const char READ[] =
  "\x48\x89\xef"          // mov    %rbp,%rdi
  "\x41\xff\xd6"          // callq  *%r14
  "\x48\x83\xf8\x00";     // cmp    $0x0,%rax
  // <inserted by code>   // jl     exit

// , [part2] ... *rbx = rax;
const char READ_STORE[] =
  "\x48\x89\x03";         // mov    %rax,(%rbx)

// . rax = write(r13, rbx); if (rax != 1) goto exit;
const char WRITE[] =
  "\x4c\x89\xef"          // mov    %r13,%rdi
  "\x48\x0f\xb6\x33"      // movzbq (%rbx),%rsi
  "\x41\xff\xd4"          // callq  *%r12
  "\x48\x83\xf8\x01";     // cmp    $0x1,%rax
  // <inserted by code>   // jne    exit

char LOOP_CMP[] =
  "\x80\x3b\x00";         // cmpb   $0x0,(%rbx)


static bool bf_write(void*, char c) {
  return putchar(c) != EOF;
};

static int bf_read(void*) {
  int c = getchar();
  if (c == EOF) {
    return 0;
  } else {
    return c;
  }
}

static bool find_loop_end(string::const_iterator loop_start,
                          string::const_iterator string_end,
                          string::const_iterator* loop_end) {
  int level = 1;
  for (string::const_iterator it=loop_start+1; it != string_end; ++it) {
    if (*it == '[') {
      level += 1;
    } else if (*it == ']') {
      level -= 1;
      if (level == 0) {
        *loop_end = it;
        return true;
      }
    }
  }
  return false;
}

void BrainfuckProgram::add_jne_to_exit(string* code) {
  *code += "\x0f\x85";                                        // jne ...
  uint32_t relative_address = exit_offset_ - (code->size() + 4);
  *code +=  string((char *) &relative_address, 4);            // ... exit
}

void BrainfuckProgram::add_jl_to_exit(string* code) {
  *code += "\x0f\x8c";                                        // jl ...
  uint32_t relative_address = exit_offset_ - (code->size() + 4);
  *code += string((char *) &relative_address, 4);             // ... exit
}

void BrainfuckProgram::add_jmp_to_offset(int offset, string* code) {
  *code += "\xe9";                                            // jmp ...
  uint32_t relative_address = offset - (code->size() + 4);
  *code += string((char *) &relative_address, 4);             // ... exit
}

void BrainfuckProgram::add_jmp_to_exit(string* code) {
  add_jmp_to_offset(exit_offset_, code);
}

bool BrainfuckProgram::generate_loop_code(string::const_iterator start,
                                          string::const_iterator end,
                                          string* code) {
  // Converts a Brainfuck instruction sequence like this:
  // [<code>]
  // Into this:
  // loop_start:
  //   cmpb   $0x0,(%rbx)
  //   je     loop_end
  //   <code>
  //   jmp    loop_start
  // loop_end:
  // 

  int loop_start = code->size();
  *code += string(LOOP_CMP, sizeof(LOOP_CMP) - 1);

  int jump_start = code->size();
  *code += string("\xde\xad\xbe\xef\xde\xad"); // Reserve 6 bytes for je.

  if (!generate_sequence_code(start+1, end, code)) {
    return false;
  }

  add_jmp_to_offset(loop_start, code);  // Jump back to the start of the loop.

  string jump_to_end = "\x0f\x84";                              // je ...
  uint32_t relative_end_of_loop = code->size() - 
      (jump_start + jump_to_end.size() + 4);
  jump_to_end += string((char *) &relative_end_of_loop, 4);     // ... loop_end

  code->replace(jump_start, jump_to_end.size(), jump_to_end);
  return true;
}

void BrainfuckProgram::generate_left_code(string* code) {
  *code += string(LEFT, sizeof(LEFT) - 1);
}

void BrainfuckProgram::generate_right_code(string* code) {
  *code += string(RIGHT, sizeof(RIGHT) - 1);
}

void BrainfuckProgram::generate_subtract_code(string* code) {
  *code += string(SUBTRACT, sizeof(SUBTRACT) - 1);
}

void BrainfuckProgram::generate_add_code(string* code) {
  *code += string(ADD, sizeof(ADD) - 1);
}

void BrainfuckProgram::generate_read_code(string* code) {
  *code += string(READ, sizeof(READ) - 1);
  add_jl_to_exit(code);
  *code += string(READ_STORE, sizeof(READ_STORE) - 1);
}

void BrainfuckProgram::generate_write_code(string* code) {
  *code += string(WRITE, sizeof(WRITE) - 1);
  add_jne_to_exit(code);
}

bool BrainfuckProgram::generate_sequence_code(string::const_iterator start,
                            string::const_iterator end,
                            string* code) {
  for (string::const_iterator it=start; it != end; ++it) {
    switch (*it) {
      case '<':
        generate_left_code(code);
        break;
      case '>':
        generate_right_code(code);
        break;
      case '-':
        generate_subtract_code(code);
        break;
      case '+':
        generate_add_code(code);
        break;
      case ',':
        generate_read_code(code);
        break;
      case '.':
        generate_write_code(code);
        break;
      case '[':
        string::const_iterator loop_end;
        if (!find_loop_end(it, end, &loop_end)) {
          fprintf(
              stderr,
              "Unable to find loop end in block starting with: %s\n",
              string(it, end).c_str());
          return false;
        }
        if (!generate_loop_code(it, loop_end, code)) {
          return false;
        }
        it = loop_end;
        break;
    }
  }
  return true;
}


BrainfuckProgram::BrainfuckProgram() : executable_(NULL) {}

bool BrainfuckProgram::init(const string& source) {
  string code(START, sizeof(START) - 1);
  code += "\xeb";  // relative jump;
  code += sizeof(EXIT) - 1;
  exit_offset_ = code.size();
  code += string(EXIT, sizeof(EXIT) - 1);

  if (!generate_sequence_code(source.begin(), source.end(), &code)) {
    return false;
  }
  add_jmp_to_exit(&code);

  int required_memory = (code.size() /
                         sysconf(_SC_PAGESIZE) + 1) * sysconf(_SC_PAGESIZE);

  executable_ = mmap(
      NULL,
      required_memory,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE|MAP_ANON, -1, 0);
  if (executable_ == NULL) {
    fprintf(stderr, "Error making memory executable: %s\n", strerror(errno));
    return false;
  }

  memmove(executable_, code.data(), code.size());
  if (mprotect(executable_, required_memory, PROT_EXEC | PROT_READ) != 0) {
    fprintf(stderr, "mprotect failed: %s\n", strerror(errno));
    return false;
  }

  return true;  
}

void BrainfuckProgram::run(void* memory) {
  ((BrainfuckFunction)executable_)(&bf_write, NULL, &bf_read, NULL, memory);
}

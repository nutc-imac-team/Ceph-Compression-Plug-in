#include "LzfseCompressor.h"
#include "include/scope_guard.h"
#include "lzfse.h"
#define MAX_LEN (CEPH_PAGE_SIZE)
#include "include/scope_guard.h"


void lzfse_deinit(char* workmem)
{
  free(workmem);
}


char* lzfse_init()
{
  return (char*) malloc(std::max<size_t>(lzfse_encode_scratch_size(), lzfse_decode_scratch_size()));
}

int LzfseCompressor::compress(const bufferlist &in, bufferlist &out)
{
  char *workmen = lzfse_init();
  auto sg = make_scope_guard([&workmen] { lzfse_deinit(workmen); });
  for (auto &i : in.buffers()) {
    const uint8_t * c_in = (uint8_t*) i.c_str();
    size_t len = i.length();
    bufferptr ptr = buffer::create_small_page_aligned(MAX_LEN);
    size_t out_len = lzfse_encode_buffer((uint8_t *)ptr.c_str(),ptr.length(),c_in,len, workmen);
    if (out_len == 0 && len != 0) {
      return -1;
    }
    out.append(ptr, 0, out_len);
  }
  return 0;
}

int LzfseCompressor::decompress(bufferlist::const_iterator &p,
                                 size_t compressed_size,
                                 bufferlist &out)
{
  char *workmen = lzfse_init();
  auto sg = make_scope_guard([&workmen] { lzfse_deinit(workmen); });
  size_t remaining = std::min<size_t>(p.get_remaining(), compressed_size);
  while (remaining) {
    bufferptr cur_ptr = p.get_current_ptr();
    uint8_t *in = (uint8_t *)cur_ptr.c_str();
    unsigned int len = cur_ptr.length();
    bufferptr ptr = buffer::create_small_page_aligned(MAX_LEN);
    uint8_t *next_out = (uint8_t *)ptr.c_str();
    size_t out_size = lzfse_decode_buffer(next_out,ptr.length(),in,len, workmen);
    if (out_size == 0 && len != 0) {
      return -1;
    }
    p.advance(remaining);
    remaining -= len;
    out.append(ptr, 0, out_size);
  }
  return 0;
}

int LzfseCompressor::decompress(const bufferlist &in, bufferlist &out)
{
  auto i = std::cbegin(in);
  return decompress(i, in.length(), out);
}

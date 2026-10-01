#include "../lzma2601/C/Aes.h"
#include "../lzma2601/C/Sha256.h"

void Z7_FASTCALL Sha256_UpdateBlocks(UInt32 state[8], const Byte *data, size_t numBlocks);

void Z7_FASTCALL AesCbc_Encode_HW(UInt32 *ivAes, Byte *data, size_t numBlocks)
{
  AesCbc_Encode(ivAes,data,numBlocks);
}

void Z7_FASTCALL AesCbc_Decode_HW(UInt32 *ivAes, Byte *data, size_t numBlocks)
{
  AesCbc_Decode(ivAes,data,numBlocks);
}

void Z7_FASTCALL AesCtr_Code_HW(UInt32 *ivAes, Byte *data, size_t numBlocks)
{
  AesCtr_Code(ivAes,data,numBlocks);
}

void Z7_FASTCALL AesCbc_Decode_HW_256(UInt32 *ivAes, Byte *data, size_t numBlocks)
{
  AesCbc_Decode(ivAes,data,numBlocks);
}

void Z7_FASTCALL AesCtr_Code_HW_256(UInt32 *ivAes, Byte *data, size_t numBlocks)
{
  AesCtr_Code(ivAes,data,numBlocks);
}

void Z7_FASTCALL Sha256_UpdateBlocks_HW(UInt32 state[8], const Byte *data, size_t numBlocks)
{
  Sha256_UpdateBlocks(state,data,numBlocks);
}

# Decoder-only 7-Zip objects used by the WFM helper. Keep update, encoder,
# benchmark, GUI, FileManager, Agent and SFX objects out of this list.
ENGINE_CPP_OBJECT_NAMES := \
  EngineGuids WfmEngine WfmPerf WfmReadAhead CRC CrcReg IntToString StringToInt MyString MyVector MyXml NewHandler \
  Sha1Prepare Sha1Reg Sha256Prepare Sha256Reg StringConvert UTFConvert \
  Xxh64Reg XzCrc64Init XzCrc64Reg \
  MyWindows FileDir FileFind FileIO FileName PropVariant PropVariantConv PropVariantUtils System TimeUtils \
  CreateCoder CWrappers FileStreams InBuffer FilterCoder LimitedStreams LockedStream \
  MethodId MethodProps OffsetStream OutBuffer ProgressUtils PropId StreamBinder StreamObjects StreamUtils UniqBlocks VirtThread \
  CoderMixer2 DummyOutStream FindSignature InStreamWithCRC ItemNameUtils \
  MultiStream OutStreamWithCRC OutStreamWithSha1 HandlerOut ParseProperties \
  7zDecode 7zExtract 7zHandler 7zHeader 7zIn 7zProperties 7zSpecStream 7zRegister \
  ArjHandler Bz2Handler CpioHandler GzHandler LzhHandler LzmaHandler PpmdHandler SplitHandler \
  XarHandler XzHandler ZstdHandler TarHandler TarHeader TarIn TarRegister CabBlockInStream CabHandler CabHeader CabIn CabRegister \
  RarHandler Rar5Handler ZipHandler ZipIn ZipItem ZipRegister \
  Bcj2Coder Bcj2Register BcjCoder BcjRegister BitlDecoder BranchMisc \
  BranchRegister ByteSwap BZip2Crc BZip2Decoder BZip2Register CopyCoder \
  CopyRegister Deflate64Register DeflateDecoder DeflateRegister DeltaFilter \
  ImplodeDecoder LzfseDecoder LzhDecoder Lzma2Decoder Lzma2Register \
  LzmaDecoder LzmaRegister LzOutWindow LzxDecoder PpmdDecoder \
  PpmdRegister PpmdZip QuantumDecoder ShrinkDecoder XpressDecoder XzDecoder \
  ZlibDecoder ZDecoder ZstdDecoder \
  Rar1Decoder Rar2Decoder Rar3Decoder Rar3Vm Rar5Decoder RarCodecsRegister \
  7zAes 7zAesRegister HmacSha1 HmacSha256 MyAes MyAesReg Pbkdf2HmacSha1 \
  WzAes ZipCrypto ZipStrong RandGen Rar20Crypto Rar5Aes RarAes \
  LoadCodecs

ENGINE_C_OBJECT_NAMES := \
  7zBuf2 7zCrc 7zCrcOpt 7zStream Aes AesOpt Alloc Bcj2 Blake2s Bra Bra86 \
  BraIA64 CpuArch Delta Lzma2Dec Lzma2DecMt LzmaDec Md5 MtCoder MtDec \
  Ppmd7 Ppmd7Dec Ppmd7aDec Ppmd8 Ppmd8Dec Sha1 Sha1Opt Sha256 Sha256Opt \
  Sha512 Sha512Opt SwapBytes Synchronization Threads Xxh64 Xz XzDec XzIn XzCrc64 XzCrc64Opt ZstdDec

ENGINE_CPP_OBJECTS := $(addprefix $(BUILD_DIR)/engine/,$(addsuffix .o,$(ENGINE_CPP_OBJECT_NAMES)))
ENGINE_C_OBJECTS := $(addprefix $(BUILD_DIR)/engine/,$(addsuffix .o,$(ENGINE_C_OBJECT_NAMES)))
ENGINE_OBJECTS := $(ENGINE_CPP_OBJECTS) $(ENGINE_C_OBJECTS)

ENGINE_SOURCE_DIRS := $(shell find ../../.. ../../../../C -type d)
vpath %.cpp $(ENGINE_SOURCE_DIRS)
vpath %.c $(ENGINE_SOURCE_DIRS)

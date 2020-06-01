#pragma once

#include "../String.h"

typedef s32 FileHandle;
typedef off_t FileOffset;

struct DirectoryIteration
{
	DIR *dir = NULL;
	struct dirent *dirent = NULL;
	String filename = "";
	bool isDirectory = false;
};

enum FileSeekRelative
{
	FILE_SEEK_RELATIVE_TO_START = SEEK_SET,
	FILE_SEEK_RELATIVE_TO_CURRENT = SEEK_CUR,
	FILE_SEEK_RELATIVE_TO_END = SEEK_END,
};

typedef s64 OpenFileFlags;
enum OpenFileFlagBits
{
	OPEN_FILE_READ_ONLY = O_RDONLY,
	OPEN_FILE_WRITE_ONLY = O_WRONLY,
	OPEN_FILE_CREATE = O_CREAT | O_TRUNC,
};

struct PlatformTime;

FileHandle OpenFile(const String &path, OpenFileFlags flags, bool *error);
bool CloseFile(FileHandle file);
String ReadFromFile(FileHandle file, s64 byteCount, bool *error);
bool WriteToFile(FileHandle file, s64 byteCount, void *buffer);
FileOffset GetFileLength(FileHandle file, bool *error);
FileOffset SeekFile(FileHandle file, FileOffset offset, FileSeekRelative relative);
bool IterateDirectory(const String &path, DirectoryIteration *context);
PlatformTime GetFileLastModifiedTime(FileHandle file, bool *error);
bool FileExists(const String &path);
bool CreateDirectoryIfItDoesNotExist(const String &path);
bool DeleteFile(const String &path);

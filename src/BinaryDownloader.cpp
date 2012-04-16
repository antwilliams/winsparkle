#include "download.h"

#include "error.h"
#include "settings.h"
#include "utils.h"
#include "winsparkle-version.h"

#include <string>
#include <windows.h>
#include <wininet.h>

#include "BinaryDownloader.h"

void BinaryDownloader::Add(const void* data,size_t len)
{
	_outputFileStream.write((const char*)data,len);
}

#ifndef _FILEDOWNLOAD_H_
#define _FILEDOWNLOAD_H_

#include <string>
#include "download.h"

#include <fstream>

class BinaryDownloader : public winsparkle::IDownloadSink
{
	virtual void Add(const void *data, size_t len);

protected:
	std::ofstream _outputFileStream;
};
#endif // _FILEDOWNLOAD_H_
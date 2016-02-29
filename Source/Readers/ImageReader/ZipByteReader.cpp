//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include <opencv2/opencv.hpp>
#include "ByteReader.h"

#ifdef USE_ZIP

namespace Microsoft { namespace MSR { namespace CNTK {

std::string GetZipError(int err)
{
    zip_error_t error;
    zip_error_init_with_code(&error, err);
    std::string errS(zip_error_strerror(&error));
    zip_error_fini(&error);
    return errS;
}

ZipByteReader::ZipByteReader(const std::string& zipPath)
    : m_zipPath(zipPath)
{
    assert(!m_zipPath.empty());
}

ZipByteReader::ZipPtr ZipByteReader::OpenZip()
{
    int err = ZIP_ER_OK;
    auto zip = zip_open(m_zipPath.c_str(), 0, &err);
    if (ZIP_ER_OK != err)
        RuntimeError("Failed to open %s, zip library error: %s", m_zipPath.c_str(), GetZipError(err).c_str());

    return ZipPtr(zip, [](zip_t* z)
    {
        assert(z != nullptr);
        int err = zip_close(z);
        assert(ZIP_ER_OK == err);
#ifdef NDEBUG
        UNUSED(err);
#endif
    });
}

void ZipByteReader::Register(size_t seqId, const std::string& path)
{
    auto zipFile = m_zips.pop_or_create([this]() { return OpenZip(); });
    zip_stat_t stat;
    zip_stat_init(&stat);
    int err = zip_stat(zipFile.get(), path.c_str(), 0, &stat);
    if (ZIP_ER_OK != err)
        RuntimeError("Failed to get file info of %s, zip library error: %s", path.c_str(), GetZipError(err).c_str());
    m_seqIdToIndex[seqId] = std::make_pair(stat.index, stat.size);
    m_zips.push(std::move(zipFile));
}

cv::Mat ZipByteReader::Read(size_t seqId, const std::string& path)
{
    // Find index of the file in .zip file.
    auto r = m_seqIdToIndex.find(seqId);
    if (r == m_seqIdToIndex.end())
        RuntimeError("Could not find file %s in the zip file, sequence id = %lu", path.c_str(), (long)seqId);

    zip_uint64_t index = std::get<0>((*r).second);
    zip_uint64_t size = std::get<1>((*r).second);

    auto contents = m_workspace.pop_or_create([size]() { return vector<unsigned char>(size); });
    if (contents.size() < size)
        contents.resize(size);
    auto zipFile = m_zips.pop_or_create([this]() { return OpenZip(); });
    {
        std::unique_ptr<zip_file_t, void(*)(zip_file_t*)> file(
            zip_fopen_index(zipFile.get(), index, 0),
            [](zip_file_t* f)
            {
                assert(f != nullptr);
                int err = zip_fclose(f);
                assert(ZIP_ER_OK == err);
#ifdef NDEBUG
                UNUSED(err);
#endif
            });
        assert(nullptr != file);
        if (nullptr == file)
        {
            RuntimeError("Could not open file %s in the zip file, sequence id = %lu, zip library error: %s",
                         path.c_str(), (long)seqId, GetZipError(zip_error_code_zip(zip_get_error(zipFile.get()))).c_str());
        }
        assert(contents.size() >= size);
        zip_uint64_t bytesRead = zip_fread(file.get(), contents.data(), size);
        assert(bytesRead == size);
        if (bytesRead != size)
        {
            RuntimeError("Bytes read %lu != expected %lu while reading file %s",
                         (long)bytesRead, (long)size, path.c_str());
        }
    }
    m_zips.push(std::move(zipFile));

    cv::Mat img = cv::imdecode(cv::Mat(1, (int)size, CV_8UC1, contents.data()), cv::IMREAD_COLOR);
    assert(nullptr != img.data);
    m_workspace.push(std::move(contents));
    return img;
}
}}}

#endif
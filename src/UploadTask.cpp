// Copyright (c) 2012-2025 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "UploadTask.h"
#include <stdlib.h>
#include "DownloadManager.h"

// 0 (zero) is an INVALID upload id...
uint32_t UploadTask::s_genid = 1;

/*
 *
 * http://wiki.developers.facebook.com/index.php/Photos.upload
 * http://www.flickr.com/services/api/upload.api.html
 *
 */

namespace {

// Fill in one multipart part. Only sets the content type when one was given:
// curl_mime_type(part, "") would emit an empty Content-Type header, whereas
// curl_formadd() skipped CURLFORM_CONTENTTYPE with an empty string.
bool addMimePart(curl_mimepart * part, const std::string& name,
                 const std::string& contentType, const std::string& data,
                 bool dataIsAFilePath)
{
    CURLcode rc;

    if ((rc = curl_mime_name(part, name.c_str())) != CURLE_OK) {
        LOG_DEBUG ("curl_mime_name() failed [%d]", rc);
        return false;
    }

    if (!contentType.empty()
            && (rc = curl_mime_type(part, contentType.c_str())) != CURLE_OK) {
        LOG_DEBUG ("curl_mime_type() failed [%d]", rc);
        return false;
    }

    if (dataIsAFilePath)
        rc = curl_mime_filedata(part, data.c_str());
    else
        rc = curl_mime_data(part, data.c_str(), CURL_ZERO_TERMINATED);

    if (rc != CURLE_OK) {
        LOG_DEBUG ("%s() failed [%d]",
                   dataIsAFilePath ? "curl_mime_filedata" : "curl_mime_data", rc);
        return false;
    }

    return true;
}

}   // namespace

//static
UploadTask * UploadTask::newFileUploadTask(const std::string& targeturl,const std::string& sourcefile,const std::string& filemimepartlabel,std::vector<PostItem>& postparts,
         std::vector<std::string>& httpheaders,std::vector<kvpair>& cookies,const std::string& contenttype)
{
    // set up the curl handle
    CURL * p_curl = curl_easy_init();
    
    if(p_curl==NULL){
         LOG_DEBUG ("p_curl is an nullptr");
         return NULL;
    }

    if (curl_easy_setopt(p_curl, CURLOPT_URL,targeturl.c_str()) != CURLE_OK ) {
        curl_easy_cleanup(p_curl);
        return NULL;
    }

    /*
     * UploadTask(const std::string& url,const std::string file,const std::string& data,uint32_t id,const std::vector<kvpair> * postparts,const std::string& contenttype,CURL * p_curl); */
    UploadTask * p_ult = new UploadTask(targeturl,sourcefile,"",UploadTask::genNewId(),&postparts,contenttype,p_curl);

    // Build the multipart body. curl_formadd()/CURLOPT_HTTPPOST have been
    // deprecated since libcurl 7.56 in favour of the mime API; the mapping is
    // CURLFORM_COPYNAME -> curl_mime_name(), CURLFORM_CONTENTTYPE ->
    // curl_mime_type(), CURLFORM_COPYCONTENTS -> curl_mime_data() and
    // CURLFORM_FILE -> curl_mime_filedata(). Both copy the name and the inline
    // data, so the std::strings here still do not have to outlive this call.
    p_ult->m_p_mime = curl_mime_init(p_curl);
    if (p_ult->m_p_mime == NULL) {
        LOG_DEBUG ("Function curl_mime_init() failed");
        delete p_ult;                   // ~UploadTask cleans up p_curl
        return NULL;
    }

    for (std::vector<PostItem>::iterator it = postparts.begin(); it != postparts.end();++it) {
        if (((*it)._type != PostItem::Value) && ((*it)._type != PostItem::File))
            continue;

        curl_mimepart * part = curl_mime_addpart(p_ult->m_p_mime);
        if (part == NULL) {
            LOG_DEBUG ("Function curl_mime_addpart() failed");
            continue;
        }

        if (!addMimePart(part, (*it)._key, (*it)._contentType,
                         (*it)._data, (*it)._type == PostItem::File))
            LOG_DEBUG ("Failed to add post part [%s]", (*it)._key.c_str());
    }

    // ...and the file itself. curl_mime_filedata() derives the part's filename
    // from the path, which is what CURLFORM_FILE did.
    curl_mimepart * filePart = curl_mime_addpart(p_ult->m_p_mime);
    if (filePart == NULL) {
        LOG_DEBUG ("Function curl_mime_addpart() failed for the file part");
    }
    else if (!addMimePart(filePart, filemimepartlabel, p_ult->m_contentType,
                          sourcefile, true)) {
        LOG_DEBUG ("Failed to add the file part for [%s]", sourcefile.c_str());
    }

    CURLcode rc = CURLE_OK;
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_MIMEPOST, p_ult->m_p_mime)) != CURLE_OK) {
        LOG_DEBUG("curl set opt: CURLOPT_MIMEPOST failed [%d]\n", rc);
    }

    // set http headers
    p_ult->setHTTPHeaders(httpheaders);

    //set all the cookies
    // curl_easy_setopt(easyhandle, CURLOPT_COOKIE, "name1=var1; name2=var2;");
    std::string cookiestr;
    for (std::vector<kvpair>::iterator it = cookies.begin(); it != cookies.end();++it) {
        cookiestr += it->first + std::string("=") + it->second + std::string("; ");
    }
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_COOKIE,cookiestr.c_str())) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_COOKIE failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_CAPATH, DOWNLOADMANAGER_TRUSTED_CERT_PATH)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_CAPATH failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_WRITEFUNCTION, DownloadManager::cbUploadResponse)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_WRITEFUNCTION failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_WRITEDATA,p_ult)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_WRITEDATA failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_WRITEHEADER,p_curl)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_WRITEHEADER failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_HEADERFUNCTION, DownloadManager::cbCurlHeaderInfo)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_HEADERFUNCTION failed [%d]\n", rc);
    // curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    return p_ult;
}

UploadTask * UploadTask::newBufferUploadTask(const std::string& targeturl,const std::string& sourcebuffer,
         std::vector<std::string>& httpheaders,const std::string& contenttype)
{
    //TODO: check parameter validity
    // LOG_DEBUG ("%s: ", __func__);

    // set up the curl handle
    CURL * p_curl = curl_easy_init();
    
    if(p_curl==NULL){
        LOG_DEBUG("p_curl is an nullptr");
        return NULL;
    }

    if (curl_easy_setopt(p_curl, CURLOPT_URL,targeturl.c_str()) != CURLE_OK ) {
        curl_easy_cleanup(p_curl);
        return NULL;
    }

    // LOG_DEBUG ("%s: url %s", __func__, targeturl.c_str());
    /*
     * UploadTask(const std::string& url,const std::string file,const std::string& data,uint32_t id,const std::vector<kvpair> * postparts,const std::string& contenttype,CURL * p_curl); */
    UploadTask * p_ult = new UploadTask(targeturl,sourcebuffer,"",UploadTask::genNewId(),NULL,contenttype,p_curl);

    CURLcode rc = CURLE_OK;
    // CURLOPT_POSTFIELDS does not copy: libcurl keeps the pointer and reads it
    // while the transfer runs, long after this caller's std::string has gone
    // out of scope. Set the size first, then COPYPOSTFIELDS, so libcurl takes
    // its own copy of exactly sourcebuffer.length() bytes (the buffer may
    // legitimately contain NULs).
    if ((rc = curl_easy_setopt (p_ult->m_p_curlHandle, CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)sourcebuffer.length())) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_POSTFIELDSIZE_LARGE failed [%d]\n", rc);
    if ((rc = curl_easy_setopt (p_ult->m_p_curlHandle, CURLOPT_COPYPOSTFIELDS, sourcebuffer.c_str())) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_COPYPOSTFIELDS failed [%d]\n", rc);
    // LOG_DEBUG ("%s: sending postfield %s with postfieldsize %d", __func__, sourcebuffer.c_str(), sourcebuffer.length());

    // set http headers
    p_ult->setHTTPHeaders(httpheaders);

    if ((rc = curl_easy_setopt(p_curl, CURLOPT_CAPATH, DOWNLOADMANAGER_TRUSTED_CERT_PATH)) != CURLE_OK)
        LOG_DEBUG("curl set opt: DOWNLOADMANAGER_TRUSTED_CERT_PATH failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_WRITEFUNCTION, DownloadManager::cbUploadResponse)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_WRITEFUNCTION failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_WRITEDATA,p_ult)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_WRITEDATA failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_WRITEHEADER,p_curl)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_WRITEHEADER failed [%d]\n", rc);
    if ((rc = curl_easy_setopt(p_curl, CURLOPT_HEADERFUNCTION, DownloadManager::cbCurlHeaderInfo)) != CURLE_OK)
        LOG_DEBUG("curl set opt: CURLOPT_HEADERFUNCTION failed [%d]\n", rc);
    // curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    return p_ult;
}

UploadTask::UploadTask()
:   m_ulid(0) ,
    m_p_curlHandle(nullptr) ,
    m_p_curlHeaderList(0) ,
    m_p_mime(0) ,
    m_curlResultCode(CURLE_OK) ,
    m_httpResultCode(0)
{
}

UploadTask::UploadTask(const std::string& url,const std::string& file,const std::string& data,uint32_t id,
        std::vector<PostItem> * postparts,const std::string& contenttype,CURL * p_curl)
:   m_url(url) ,
    m_sourceFile(file) ,
    m_sourceData(data) ,
    m_ulid(id) ,
    m_contentType(contenttype) ,
    m_p_curlHandle(p_curl) ,
    m_p_curlHeaderList(0) ,
    m_p_mime(0) ,
    m_curlResultCode(CURLE_OK) ,
    m_httpResultCode(0)
{
        if (postparts)
            m_postParts = *postparts;
}

void UploadTask::setHTTPHeaders(std::vector<std::string>& headerList)
{
    if (m_p_curlHandle == NULL)
        return;

    //clear existing headers
    if (m_p_curlHeaderList) {
        curl_slist_free_all(m_p_curlHeaderList);
        m_p_curlHeaderList = 0;
    }

    for (std::vector<std::string>::iterator it = headerList.begin();it != headerList.end();++it)
        m_p_curlHeaderList = curl_slist_append(m_p_curlHeaderList,(*it).c_str());

    CURLcode rc = CURLE_OK;
    if ((rc = curl_easy_setopt(m_p_curlHandle, CURLOPT_HTTPHEADER, m_p_curlHeaderList)) != CURLE_OK) {
        LOG_DEBUG("curl set opt: CURLOPT_HTTPHEADER failed [%d]\n", rc);
    }
 }

UploadTask::~UploadTask()
{
    if (m_p_curlHeaderList) {
        curl_slist_free_all(m_p_curlHeaderList);
    }
    // The mime structure belongs to the easy handle it was created for, so it
    // has to go first: using or freeing it after curl_easy_cleanup() is
    // undefined.
    if (m_p_mime) {
        curl_mime_free(m_p_mime);
        m_p_mime = 0;
    }

    curl_easy_cleanup(m_p_curlHandle);
}

//TODO: moved here temporarily to hack in cancel support for uploads. Move back to it's logical location
uint32_t UploadTask::genNewId()
{
    return (DownloadManager::instance().generateNewTicket());
}



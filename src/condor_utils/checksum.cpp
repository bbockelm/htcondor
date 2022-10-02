/***************************************************************
 *
 * Copyright (C) 2022, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_debug.h"
#include "condor_system.h"

#include <string>
#include <map>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <ssl_version_hack.h>

#include "safe_open.h"
#include "AWSv4-impl.h"


bool
compute_sha256_checksum(int fd, std::string & checksum, CondorError &err) {
    const size_t BUF_SIZ = 1024 * 1024;
    std::unique_ptr<unsigned char, decltype(free)>
        buffer((unsigned char *)calloc(BUF_SIZ, 1), &free);
    ASSERT(buffer);

    std::unique_ptr<EVP_MD_CTX, decltype(condor_EVP_MD_CTX_free)>
        context(condor_EVP_MD_CTX_new(), condor_EVP_MD_CTX_free);

    if (!context || !buffer) {
        err.push("SHA256_CHECKSUM", 1, "Failed to allocate checksum buffers");
        return false;
    }
    if (!EVP_DigestInit_ex(context.get(), EVP_sha256(), NULL)) {
	err.push("SHA256_CHECKSUM", 1, "Failed to initialize SHA256 checksum object");
        return false;
    }

    ssize_t bytesRead = full_read(fd, buffer.get(), BUF_SIZ);
        // Per OpenSSL docs: return 1 for success and 0 for failure
    int digest_result = 1;
    while (bytesRead > 0) {
        if (0 == (digest_result = EVP_DigestUpdate(context.get(), buffer.get(), bytesRead))) {
            break;
        }
        bytesRead = full_read(fd, buffer.get(), BUF_SIZ);
    }
    if (bytesRead < 0) {
        err.pushf("SHA256_CHECKSUM", 2, "Failure when reading checksum input: %s (errno=%d)",
            strerror(errno), errno);
        return false;
    }
    if (digest_result == 0) {
        err.push("SHA256_CHECKSUM", 3, "Failure when computing SHA256 digest");
        return false;
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(context, hash, NULL)) {
        err.push("SHA256_CHECKSUM", 4, "Failure when finalizing the SHA256 digest");
        return false;
    }

    AWSv4Impl::convertMessageDigestToLowercaseHex(
        hash, SHA256_DIGEST_LENGTH, checksum
    );
    return true;
}


bool
compute_file_sha256_checksum(const std::string & file_name, std::string & checksum, CondorError &err) {
    auto fd = safe_open_wrapper_follow(file_name.c_str(), O_RDONLY | O_LARGEFILE | _O_BINARY, 0);
    if (fd < 0) {
        err.pushf("SHA256_CHECKSUM", 5, "Failed to open '%s' for checksum: %s (errno=%d)",
            file_name.c_str(), strerror(errno), errno);
        return false;
    }
    auto rv = compute_sha256_checksum(fd, checksum);
    close(fd);
    return rv;
}

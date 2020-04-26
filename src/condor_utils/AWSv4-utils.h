#ifndef AWSV4_UTILS_H
#define AWSV4_UTILS_H

#include <string>
#include <map>

class CondorError;

namespace htcondor {
//
// The jobAd must have ATTR_EC2_ACCESS_KEY_ID ATTR_EC2_SECRET_ACCESS_KEY set.
// Optionally, ATTR_AWS_REGION may be provided.
//
bool
generate_presigned_url( const classad::ClassAd & jobAd,
	const std::string & s3url,
	const std::string & verb,
        const std::map<std::string, std::string> & query_parameters,
	std::string & presignedURL,
	CondorError & err );
}

// Functions used internally by generate_presigned_url() and the Amazon GAHP.

namespace AWSv4Impl {

std::string
pathEncode( const std::string & original );

std::string
amazonURLEncode( const std::string & input );

std::string
canonicalizeQueryString( const std::map< std::string, std::string > & qp );

void
convertMessageDigestToLowercaseHex( const unsigned char * messageDigest,
	unsigned int mdLength, std::string & hexEncoded );

bool
doSha256( const std::string & payload, unsigned char * messageDigest,
	unsigned int * mdLength );

bool
readShortFile( const std::string & fileName, std::string & contents );

bool
createSignature( const std::string & secretAccessKey,
    const std::string & date, const std::string & region,
    const std::string & service, const std::string & stringToSign,
    std::string & signature );

bool
parseS3URL(const std::string &url, const std::string &input_region,
     std::string &host, std::string &region, std::string &bucket,
     std::string &key, std::string &canonicalURI, CondorError &err);

}

#endif /* AWSV4_UTILS_H */

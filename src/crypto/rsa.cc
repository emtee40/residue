//
//  rsa.cc
//  Residue
//
//  Copyright © 2017 Muflihun Labs
//

#include "deps/ripe/Ripe.h"
#ifdef RESIDUE_USE_MINE
#include "deps/mine/mine.h"
#endif
#include "include/log.h"
#include "src/crypto/rsa.h"
#include "src/crypto/base64.h"

using namespace residue;
#ifdef RESIDUE_USE_MINE
using namespace mine;

static RSAManager rsaManager;
#endif

std::string RSA::encrypt(const std::string& data, const PublicKey& publicKey)
{
#ifdef RESIDUE_USE_MINE
    try {
        /*mine::PublicKey publicKey;
        publicKey.loadFromPem(publicKeyPEM);*/
        return rsaManager.encrypt(&publicKey, data);
    } catch (const std::exception& e) {
        RLOG(FATAL) << "Failed to encrypt (RSA) " << e.what();
        return "";
    }
#else
    return Ripe::encryptRSA(data, publicKey);
#endif
}

std::string RSA::decrypt(std::string& data, const PrivateKey& privateKey, const std::string& secret)
{

#ifdef RESIDUE_USE_MINE
    try {
        (void) secret;
        /*mine::PrivateKey privateKey;
        privateKey.loadFromPem(key, secret);*/
        std::string result = rsaManager.decrypt<std::string>(&privateKey, data);
        return result;
    } catch (const std::exception& e) {
        RLOG(FATAL) << "Failed to decrypt (RSA) " << e.what();
        return "";
    }
#else
    return Ripe::decryptRSA(data, privateKey, true, false, secret);
#endif
}

std::string RSA::sign(const std::string& data, const PrivateKey& key, const std::string& secret)
{
#ifdef RESIDUE_USE_MINE
    return Ripe::signRSA(data, key.pem(), secret);
#else
    return Ripe::signRSA(data, key, secret);
#endif

}

bool RSA::verify(const std::string& data, const std::string& signHex, const PublicKey& key)
{
#ifdef RESIDUE_USE_MINE
    return Ripe::verifyRSA(data, signHex, key.pem());
#else
    return Ripe::verifyRSA(data, signHex, key);
#endif
}

bool RSA::verifyKeyPair(const PrivateKey& privateKey, const PublicKey& publicKey, const std::string& secret)
{
    try {
#ifdef RESIDUE_USE_MINE
        (void) secret;
        return privateKey.n() == publicKey.n() && privateKey.e() == publicKey.e();
#else
        std::string testData = "test_rsa";
        std::string encr = RSA::encrypt(testData, publicKey);
        std::string decr = Ripe::decryptRSA(encr, privateKey, false, false, secret);
        return testData == decr;
#endif
    } catch (const std::exception& e) {
        return false;
    }
}

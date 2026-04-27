#ifndef PANENCRYPTED_H
#define PANENCRYPTED_H

#include <string>

class PANEncryptionService {
public:
  static PANEncryptionService &getInstance();

  std::string encryptPAN(const std::string &plainPan);
  std::string decryptPAN(const std::string &encryptedPan);
  std::string maskPAN(const std::string &pan);

private:
  PANEncryptionService() = default;

  std::string base64_encode(const unsigned char *input, int length);
  std::string base64_decode(const std::string &input);
};

#endif
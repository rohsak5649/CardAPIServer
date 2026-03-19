#ifndef PIN_H
#define PIN_H

#include <string>
#include <vector>

class PINService {
public:
    // ===================== SINGLETON =====================
    static PINService& getInstance();

    // ===================== PUBLIC APIs =====================
    std::string generatePIN(const std::string& pan, size_t length = 150) const;

    bool verifyPIN(const std::string& pan, const std::string& inputPin) const;

private:
    // ===================== CONSTRUCTOR =====================
    PINService();                          // private constructor
    PINService(const PINService&) = delete;
    PINService& operator=(const PINService&) = delete;

    // ===================== SECRET =====================
    std::string secretKey;

    // ===================== INTERNAL UTILS =====================
    unsigned long long rotl(unsigned long long x, int r) const;
    unsigned long long avalanche(unsigned long long x) const;

    std::vector<unsigned char> hmacSha256(const std::string& data) const;

    unsigned long long compress(const std::vector<unsigned char>& v) const;

    std::vector<unsigned long long> derive(const std::string& pan) const;
};

#endif
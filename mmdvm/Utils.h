#ifndef Utils_H
#define Utils_H

#include <string>

class CUtils {
public:
	static void dump(const std::string& title, const unsigned char* data, unsigned int length);
	static void dump(int level, const std::string& title, const unsigned char* data, unsigned int length);

	static void dump(const std::string& title, const bool* bits, unsigned int length);
	static void dump(int level, const std::string& title, const bool* bits, unsigned int length);

	static void byteToBitsBE(unsigned char byte, bool* bits);
	static void byteToBitsLE(unsigned char byte, bool* bits);

	static void bitsToByteBE(const bool* bits, unsigned char& byte);
	static void bitsToByteLE(const bool* bits, unsigned char& byte);
};

#endif

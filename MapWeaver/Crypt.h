#pragma once
#include <windows.h>
#include <windef.h>
#include <wincrypt.h>

class SignatureCrypt
{
public:
	//构造函数，nFlag为真会生成随机签名密钥
	SignatureCrypt(bool bFlag = false);
	~SignatureCrypt();

	//base64编码
	static void Base64Decode(LPCSTR in, DWORD inLen, LPBYTE* out, DWORD* outLen);
	static void Base64Encode(LPBYTE in, DWORD inLen, LPSTR* out, DWORD* outLen);

	// CRC32
	static unsigned int GetCRC32Code(const void* buf, unsigned int size_in_byte, unsigned int init_crc); // Added by ZWSOFT duhu 2021/12/06, 计算CRC32

	//hash函数，默认sha256算法
	HCRYPTHASH Hash(LPBYTE message, DWORD messageLen, ALG_ID AlgId = CALG_SHA_256)const;

	//签名算法
	BOOL MessageSignature(LPBYTE message, DWORD msgLen, LPBYTE* pSign, DWORD* dwSignLen)const;

	//验证签名
	BOOL VerifySignature(LPBYTE msg, DWORD msgLen, LPBYTE sig, DWORD sigLen);

	//重新生成密钥
	void GenKey();

	//导出编码后的公钥信息
	BOOL ExportEncodePublicKeyInfo(LPBYTE* pubKey, DWORD* pubKeyLen)const;

	//导入编码后的公钥信息
	BOOL ImportPublicKeyInfo(LPBYTE pubKeyInfo, DWORD pkLen);

	//导出密钥对
	BOOL ExportKey(LPSTR* key, DWORD* len);

	//导入密钥对
	BOOL ImportKey(LPCSTR key, DWORD len);

	//是否有密钥
	BOOL HasKey();

private:
	HCRYPTPROV hProv;	//容器句柄
	HCRYPTKEY hKey;		//密钥句柄
};

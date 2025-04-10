#include "Crypt.h"
#include <corecrt_malloc.h>

//[ Added by ZWSOFT duhu 2021/12/06
// 构建CRC32查询表
unsigned int crc32_table[256] = { 0 };
bool crc32_table_inited = false;
void Generate_CRC32_Table()
{
	if (crc32_table_inited)
	{
		return;
	}
	unsigned int crc_reg = 0; // 32bit
	for (int i = 0; i < 256; ++i)
	{
		crc_reg = i;
		for (int j = 0; j < 8; ++j)
		{
			if (crc_reg & 1) // LSM
			{
				crc_reg = (crc_reg >> 1) ^ 0xEDB88320; // Reversed
			}
			else
			{
				crc_reg >>= 1;
			}
		}
		crc32_table[i] = crc_reg;
	}
	crc32_table_inited = true;
}
//] Added by ZWSOFT duhu 2021/12/06

SignatureCrypt::SignatureCrypt(bool bFlag)
{
	if (!CryptAcquireContextA(&hProv, "ZWSOFT", NULL, PROV_RSA_AES, 0))
	{
		if (!CryptAcquireContextA(&hProv, "ZWSOFT", NULL, PROV_RSA_AES, CRYPT_NEWKEYSET))
		{
			//[Added by Zwsoft wuzhengyu 2020/12/03 for ZWCAD-21862
			//已经存在容器拒绝访问的情况新创建一个容器，主要是为了修复之前无法激活的用户，修改权限之后应该不会出现这种情况了
			//hProv = NULL;
			if (!CryptAcquireContextA(&hProv, "ZWSOFT1", NULL, PROV_RSA_AES, 0))
			{
				if (!CryptAcquireContextA(&hProv, "ZWSOFT1", NULL, PROV_RSA_AES, CRYPT_NEWKEYSET))
				{
					hProv = NULL;
				}
			}
			//]Added by Zwsoft wuzhengyu 2020/12/03
		}
		//[Added by Zwsoft wuzhengyu 2020/12/03 for ZWCAD-21862
		//容器存在拒绝访问的情况，这里设置所有用户都可访问
		else
		{
			SECURITY_DESCRIPTOR sd;
			if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
			{
				if (SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
				{
					CryptSetProvParam(hProv, PP_KEYSET_SEC_DESCR, (BYTE*)&sd, DACL_SECURITY_INFORMATION);
				}
			}
		}
		//]Added by Zwsoft wuzhengyu 2020/12/03
	}

	hKey = NULL;
	if (hProv && bFlag)
	{
		GenKey();
	}
}

SignatureCrypt::~SignatureCrypt()
{
	if (hKey)
	{
		CryptDestroyKey(hKey);
		hKey = NULL;
	}

	if (hProv)
	{
		CryptReleaseContext(hProv, 0);
		hProv = NULL;
	}
}

void SignatureCrypt::Base64Decode(LPCSTR in, DWORD inLen, LPBYTE* out, DWORD* outLen)
{
	DWORD len = 0;
	if (out && in && CryptStringToBinaryA(in, inLen, CRYPT_STRING_BASE64_ANY, NULL, &len, NULL, NULL))
	{
		if (!*out)
		{
			*out = new BYTE[len + 1];
			ZeroMemory(*out, len + 1);
		}

		if (CryptStringToBinaryA(in, inLen, CRYPT_STRING_BASE64_ANY, *out, &len, NULL, NULL))
		{
			*outLen = len;
		}
	}
}

void SignatureCrypt::Base64Encode(LPBYTE in, DWORD inLen, LPSTR* out, DWORD* outLen)
{
	DWORD len = 0;
	if (out && in && CryptBinaryToStringA(in, inLen, CRYPT_STRING_BASE64_ANY, NULL, &len))
	{
		if (!*out)
		{
			*out = new char[len + 1];
			ZeroMemory(*out, len + 1);
		}

		if (CryptBinaryToStringA(in, inLen, CRYPT_STRING_BASE64_ANY, *out, &len))
		{
			*outLen = len;
		}
	}
}

//[ Added by ZWSOFT duhu 2021/12/06
//  计算CRC32
unsigned int SignatureCrypt::GetCRC32Code(const void* buf, unsigned int size_in_byte, unsigned int init_crc)
{
	Generate_CRC32_Table();

	const unsigned char* p = (unsigned char*)buf;

	unsigned int crc_reg = init_crc ^ ~0U;

	while (size_in_byte--)
	{
		crc_reg = crc32_table[(crc_reg ^ *p++) & 0xFF] ^ (crc_reg >> 8);
	}

	return crc_reg ^ ~0U;
}
//} Added by ZWSOFT duhu 2021/12/06

BOOL SignatureCrypt::ExportKey(LPSTR* key, DWORD* len)
{
	if (!key)
	{
		return FALSE;
	}

	DWORD keyOrgLen = 0;
	//得到长度
	if (!CryptExportKey(hKey, NULL, PRIVATEKEYBLOB, 0, NULL, &keyOrgLen))
	{
		return FALSE;
	}

	BYTE* keyOrg = new BYTE[keyOrgLen + 1];
	//导出
	if (!CryptExportKey(hKey, NULL, PRIVATEKEYBLOB, 0, keyOrg, &keyOrgLen))
	{
		delete[]keyOrg;
		keyOrg = NULL;
		return FALSE;
	}

	//编码
	Base64Encode(keyOrg, keyOrgLen, key, len);

	delete[]keyOrg;
	keyOrg = NULL;

	return TRUE;
}

BOOL SignatureCrypt::ImportKey(LPCSTR key, DWORD len)
{
	if (!key)
	{
		return FALSE;
	}

	if (hKey)
	{
		CryptDestroyKey(hKey);
		hKey = NULL;
	}

	BYTE* keyDe = NULL;
	DWORD keyLen = 0;
	Base64Decode(key, len, &keyDe, &keyLen);
	return CryptImportKey(hProv, keyDe, keyLen, NULL, CRYPT_EXPORTABLE, &hKey);
}

BOOL SignatureCrypt::HasKey()
{
	return hKey ? TRUE : FALSE;
}

HCRYPTHASH SignatureCrypt::Hash(LPBYTE message, DWORD messageLen, ALG_ID AlgId) const
{
	HCRYPTHASH hHash = NULL;
	if (!message)
	{
		return NULL;
	}

	if (!CryptCreateHash(hProv, AlgId, 0, 0, &hHash))
	{
		return NULL;
	}

	if (!CryptHashData(hHash, message, messageLen, 0))
	{
		return NULL;
	}

	return hHash;
}

BOOL SignatureCrypt::MessageSignature(LPBYTE message, DWORD msgLen, LPBYTE* pSign, DWORD* dwSignLen) const
{
	HCRYPTHASH hMessageHash = Hash(message, msgLen);
	if (!hMessageHash || !pSign)
	{
		return FALSE;
	}

	DWORD len = 0;
	if (!CryptSignHash(hMessageHash, AT_SIGNATURE, NULL, 0, NULL, &len))
	{
		return FALSE;
	}

	*pSign = new BYTE[len + 1];
	if (!CryptSignHash(hMessageHash, AT_SIGNATURE, NULL, 0, *pSign, &len))
	{
		delete[] * pSign;
		pSign = NULL;
		return FALSE;
	}

	*dwSignLen = len;
	if (hMessageHash)
	{
		CryptDestroyHash(hMessageHash);
	}
	return TRUE;
}

BOOL SignatureCrypt::ExportEncodePublicKeyInfo(LPBYTE* pubKey, DWORD* pubKeyLen) const
{
	if (!pubKey)
	{
		return FALSE;
	}

	*pubKeyLen = 0;
	DWORD pubKeyInfoLen = 0;
	//公钥长度
	if (!CryptExportPublicKeyInfo(hProv, AT_SIGNATURE, X509_ASN_ENCODING, NULL, &pubKeyInfoLen))
	{
		return FALSE;
	}

	CERT_PUBLIC_KEY_INFO* publicKeyInfo = (CERT_PUBLIC_KEY_INFO*)malloc(pubKeyInfoLen + 1);
	//导出公钥
	if (!CryptExportPublicKeyInfo(hProv, AT_SIGNATURE, X509_ASN_ENCODING, publicKeyInfo, &pubKeyInfoLen))
	{
		free(publicKeyInfo);
		publicKeyInfo = NULL;
		return FALSE;
	}

	//编码长度
	if (!CryptEncodeObjectEx((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING), X509_PUBLIC_KEY_INFO,
		publicKeyInfo, 0, NULL, NULL, pubKeyLen))
	{
		free(publicKeyInfo);
		publicKeyInfo = NULL;
		return FALSE;
	}

	*pubKey = new BYTE[(*pubKeyLen) + 1];
	//编码
	if (!CryptEncodeObjectEx((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING), X509_PUBLIC_KEY_INFO,
		publicKeyInfo, 0, NULL, *pubKey, pubKeyLen))
	{
		free(publicKeyInfo);
		publicKeyInfo = NULL;
		delete[] * pubKey;
		*pubKey = NULL;
		return FALSE;
	}

	free(publicKeyInfo);
	publicKeyInfo = NULL;

	return TRUE;
}

BOOL SignatureCrypt::ImportPublicKeyInfo(LPBYTE pubKeyInfo, DWORD len)
{
	if (!pubKeyInfo)
	{
		return FALSE;
	}

	if (hKey)
	{
		CryptDestroyKey(hKey);
		hKey = NULL;
	}

	CERT_PUBLIC_KEY_INFO* publicKeyInfo;
	DWORD publicKeyInfoLen = 0;
	//解码
	if (CryptDecodeObjectEx((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING), X509_PUBLIC_KEY_INFO,
		pubKeyInfo, len,
		CRYPT_DECODE_ALLOC_FLAG, NULL,
		&publicKeyInfo, &publicKeyInfoLen))
	{
		//导入
		BOOL nReturn = CryptImportPublicKeyInfo(hProv, X509_ASN_ENCODING, publicKeyInfo, &hKey);
		LocalFree(publicKeyInfo);
		publicKeyInfo = NULL;
		return nReturn;
	}
	return FALSE;
}

BOOL SignatureCrypt::VerifySignature(LPBYTE msg, DWORD msgLen, LPBYTE sig, DWORD sigLen)
{
	BOOL rc = FALSE;
	if (msg && sig && hKey)
	{
		HCRYPTHASH hMessageHash = NULL;
		hMessageHash = Hash(msg, msgLen);
		if (hMessageHash)
		{
			//验证
			rc = CryptVerifySignature(hMessageHash, sig, sigLen, hKey, NULL, 0);
			CryptDestroyHash(hMessageHash);
		}
	}

	return rc;
}

void SignatureCrypt::GenKey()
{
	if (hKey)
	{
		CryptDestroyKey(hKey);
	}

	if (!CryptGenKey(hProv, AT_SIGNATURE, CRYPT_EXPORTABLE | 0x10000000, &hKey))
	{
		throw "生成密钥失败";
	}
}
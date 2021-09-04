﻿/*=============================================================================
*
*									ソケット
*
===============================================================================
/ Copyright (C) 1997-2007 Sota. All rights reserved.
/
/ Redistribution and use in source and binary forms, with or without 
/ modification, are permitted provided that the following conditions 
/ are met:
/
/  1. Redistributions of source code must retain the above copyright 
/     notice, this list of conditions and the following disclaimer.
/  2. Redistributions in binary form must reproduce the above copyright 
/     notice, this list of conditions and the following disclaimer in the 
/     documentation and/or other materials provided with the distribution.
/
/ THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR 
/ IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
/ OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
/ IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, 
/ INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
/ BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF 
/ USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON 
/ ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
/ (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF 
/ THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/============================================================================*/

#include "common.h"
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Cryptui.lib")
#pragma comment(lib, "Secur32.lib")


#define USE_THIS	1
#define DBG_MSG		0

static LRESULT CALLBACK SocketWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
extern int TimeOut;


/*===== ローカルなワーク =====*/

static HWND hWndSocket;
static std::map<SOCKET, SocketContext*> map;
static std::mutex mapMutex;
static constexpr unsigned long contextReq = ISC_REQ_STREAM | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_MANUAL_CRED_VALIDATION;
static CredHandle credential = CreateInvalidateHandle<CredHandle>();


void SocketContext::Decypt() {
	while (!empty(readRaw)) {
		if (sslNeedRenegotiate) {
			SecBuffer inBuffer[]{ { size_as<unsigned long>(readRaw), SECBUFFER_TOKEN, data(readRaw) }, { 0, SECBUFFER_EMPTY, nullptr } };
			SecBuffer outBuffer[]{ { 0, SECBUFFER_EMPTY, nullptr }, { 0, SECBUFFER_EMPTY, nullptr } };
			SecBufferDesc inDesc{ SECBUFFER_VERSION, size_as<unsigned long>(inBuffer), inBuffer };
			SecBufferDesc outDesc{ SECBUFFER_VERSION, size_as<unsigned long>(outBuffer), outBuffer };
			unsigned long attr = 0;
			sslReadStatus = InitializeSecurityContextW(&credential, &sslContext, const_cast<SEC_WCHAR*>(punyTarget.c_str()), contextReq, 0, 0, &inDesc, 0, nullptr, &outDesc, &attr, nullptr);
			_RPTWN(_CRT_WARN, L"Decypt(): InitializeSecurityContextW(): in=%d, 0x%08X, in: %d/%d/%p, %d/%d/%p, out: %d/%d/%p, %d/%d/%p, attr=%X.\n", size_as<int>(readRaw), sslReadStatus,
				inBuffer[0].BufferType, inBuffer[0].cbBuffer, inBuffer[0].pvBuffer, inBuffer[1].BufferType, inBuffer[1].cbBuffer, inBuffer[1].pvBuffer,
				outBuffer[0].BufferType, outBuffer[0].cbBuffer, outBuffer[0].pvBuffer, outBuffer[1].BufferType, outBuffer[1].cbBuffer, outBuffer[1].pvBuffer,
				attr
			);
			if (outBuffer[0].BufferType == SECBUFFER_TOKEN && outBuffer[0].cbBuffer != 0) {
				auto written = send(handle, reinterpret_cast<const char*>(outBuffer[0].pvBuffer), outBuffer[0].cbBuffer, 0);
				_RPTWN(_CRT_WARN, L"Decypt(): send: %d bytes.\n", written);
				assert(written == outBuffer[0].cbBuffer);
				FreeContextBuffer(outBuffer[0].pvBuffer);
			}
			if (sslReadStatus == SEC_E_OK || sslReadStatus == SEC_I_CONTINUE_NEEDED) {
				readRaw.erase(begin(readRaw), end(readRaw) - (inBuffer[1].BufferType == SECBUFFER_EXTRA ? inBuffer[1].cbBuffer : 0));
				if (sslReadStatus == SEC_E_OK)
					sslNeedRenegotiate = false;
			} else {
				if (sslReadStatus != SEC_E_INCOMPLETE_MESSAGE)
					Error(L"Decypt(): InitializeSecurityContextW()"sv, sslReadStatus);
				return;
			}
		} else {
			SecBuffer buffer[]{
				{ size_as<unsigned long>(readRaw), SECBUFFER_DATA, data(readRaw) },
				{ 0, SECBUFFER_EMPTY, nullptr },
				{ 0, SECBUFFER_EMPTY, nullptr },
				{ 0, SECBUFFER_EMPTY, nullptr },
			};
			SecBufferDesc desc{ SECBUFFER_VERSION, size_as<unsigned long>(buffer), buffer };
			sslReadStatus = DecryptMessage(&sslContext, &desc, 0, nullptr);
			_RPTWN(_CRT_WARN, L"DecryptMessage(): in=%d, %08X, %d/%d/%p, %d/%d/%p, %d/%d/%p, %d/%d/%p.\n", size_as<int>(readRaw), sslReadStatus,
				buffer[0].BufferType, buffer[0].cbBuffer, buffer[0].pvBuffer, buffer[1].BufferType, buffer[1].cbBuffer, buffer[1].pvBuffer,
				buffer[2].BufferType, buffer[2].cbBuffer, buffer[2].pvBuffer, buffer[3].BufferType, buffer[3].cbBuffer, buffer[3].pvBuffer
			);
			if (sslReadStatus == SEC_E_OK) {
				assert(buffer[0].BufferType == SECBUFFER_STREAM_HEADER && buffer[1].BufferType == SECBUFFER_DATA && buffer[2].BufferType == SECBUFFER_STREAM_TRAILER);
				readPlain.insert(end(readPlain), reinterpret_cast<const char*>(buffer[1].pvBuffer), reinterpret_cast<const char*>(buffer[1].pvBuffer) + buffer[1].cbBuffer);
				readRaw.erase(begin(readRaw), end(readRaw) - (buffer[3].BufferType == SECBUFFER_EXTRA ? buffer[3].cbBuffer : 0));
			} else if (sslReadStatus == SEC_I_RENEGOTIATE) {
				assert(buffer[0].BufferType == SECBUFFER_STREAM_HEADER && buffer[1].BufferType == SECBUFFER_DATA && buffer[1].cbBuffer == 0 && buffer[2].BufferType == SECBUFFER_STREAM_TRAILER && buffer[3].BufferType == SECBUFFER_EXTRA);
				readRaw.erase(begin(readRaw), end(readRaw) - buffer[3].cbBuffer);
				sslNeedRenegotiate = true;
			} else {
				if (sslReadStatus != SEC_E_INCOMPLETE_MESSAGE)
					Error(L"Decypt(): DecryptMessage()"sv, sslReadStatus);
				return;
			}
		}
	}
}

std::vector<char> SocketContext::Encrypt(std::string_view plain) {
	std::vector<char> result;
	while (!empty(plain)) {
		auto dataLength = std::min(size_as<unsigned long>(plain), sslStreamSizes.cbMaximumMessage);
		auto offset = size(result);
		result.resize(offset + sslStreamSizes.cbHeader + dataLength + sslStreamSizes.cbTrailer);
		std::copy_n(begin(plain), dataLength, begin(result) + offset + sslStreamSizes.cbHeader);
		SecBuffer buffer[]{
			{ sslStreamSizes.cbHeader,  SECBUFFER_STREAM_HEADER,  data(result) + offset                                        },
			{ dataLength,               SECBUFFER_DATA,           data(result) + offset + sslStreamSizes.cbHeader              },
			{ sslStreamSizes.cbTrailer, SECBUFFER_STREAM_TRAILER, data(result) + offset + sslStreamSizes.cbHeader + dataLength },
			{ 0,                        SECBUFFER_EMPTY,          nullptr                                                      },
		};
		SecBufferDesc desc{ SECBUFFER_VERSION, size_as<unsigned long>(buffer), buffer };
		if (auto ss = EncryptMessage(&sslContext, 0, &desc, 0); ss != SEC_E_OK) {
			_RPTWN(_CRT_WARN, L"FTPS_send EncryptMessage error: %08x.\n", ss);
			return {};
		}
		assert(buffer[0].BufferType == SECBUFFER_STREAM_HEADER && buffer[0].cbBuffer == sslStreamSizes.cbHeader);
		assert(buffer[1].BufferType == SECBUFFER_DATA && buffer[1].cbBuffer == dataLength);
		assert(buffer[2].BufferType == SECBUFFER_STREAM_TRAILER && buffer[2].cbBuffer <= sslStreamSizes.cbTrailer);
		result.resize(offset + buffer[0].cbBuffer + buffer[1].cbBuffer + buffer[2].cbBuffer);
		plain = plain.substr(dataLength);
	}
	return result;
}

BOOL LoadSSL() {
	// 目的：
	//   TLS 1.1以前を無効化する動きに対応しつつ、古いSSL 2.0にもできるだけ対応する
	// 前提：
	//   Windows 7以前はTLS 1.1、TLS 1.2が既定で無効化されている。 <https://docs.microsoft.com/en-us/windows/desktop/SecAuthN/protocols-in-tls-ssl--schannel-ssp->
	//   それとは別にTLS 1.2とSSL 2.0は排他となる。
	//   ドキュメントに記載されていないUNI; Multi-Protocol Unified Helloが存在し、Windows XPではUNIが既定で有効化されている。さらにTLS 1.2とUNIは排他となる。
	//   TLS 1.3はWindows 10で実験的搭載されている。TLS 1.3を有効化するとTLS 1.3が使われなくてもセッション再開（TLS Resumption）が無効化される模様？
	// 手順：
	//   未指定でオープンすることで、レジストリ値に従った初期化をする。
	//   有効になっているプロトコルを調べ、SSL 2.0が無効かつTLS 1.2が無効な場合は開き直す。
	//   排他となるプロトコルがあるため、有効になっているプロトコルのうちSSL 3.0以降とTLS 1.2を指定してオープンする。
	//   セッション再開が必要とされるため、TLS 1.3は明示的には有効化せず、レジストリ指定に従う。
	static_assert(SP_PROT_TLS1_3PLUS_CLIENT == SP_PROT_TLS1_3_CLIENT, "new tls version detected.");
	if (auto ss = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, nullptr, nullptr, nullptr, &credential, nullptr); ss != SEC_E_OK) {
		Error(L"AcquireCredentialsHandle()"sv, ss);
		return FALSE;
	}
	SecPkgCred_SupportedProtocols sp;
	if (__pragma(warning(suppress:6001)) auto ss = QueryCredentialsAttributesW(&credential, SECPKG_ATTR_SUPPORTED_PROTOCOLS, &sp); ss != SEC_E_OK) {
		Error(L"QueryCredentialsAttributes(SECPKG_ATTR_SUPPORTED_PROTOCOLS)"sv, ss);
		return FALSE;
	}
	if ((sp.grbitProtocol & SP_PROT_SSL2_CLIENT) == 0 && (sp.grbitProtocol & SP_PROT_TLS1_2_CLIENT) == 0) {
		FreeCredentialsHandle(&credential);
		// pAuthDataはSCHANNEL_CREDからSCH_CREDENTIALSに変更されたが現状維持する。
		// https://github.com/MicrosoftDocs/win32/commit/e9f333c14bad8fd65d89ccc64d42882bc5fa7d9c
		SCHANNEL_CRED sc{ .dwVersion = SCHANNEL_CRED_VERSION, .grbitEnabledProtocols = sp.grbitProtocol & SP_PROT_SSL3TLS1_X_CLIENTS | SP_PROT_TLS1_2_CLIENT };
		if (auto ss = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &credential, nullptr); ss != SEC_E_OK) {
			Error(L"AcquireCredentialsHandle()"sv, ss);
			return FALSE;
		}
	}
	return TRUE;
}

void FreeSSL() {
	assert(SecIsValidHandle(&credential));
	FreeCredentialsHandle(&credential);
}

namespace std {
	template<>
	struct default_delete<CERT_CONTEXT> {
		void operator()(CERT_CONTEXT* ptr) {
			CertFreeCertificateContext(ptr);
		}
	};
	template<>
	struct default_delete<const CERT_CHAIN_CONTEXT> {
		void operator()(const CERT_CHAIN_CONTEXT* ptr) {
			CertFreeCertificateChain(ptr);
		}
	};
}

auto getCertContext(CtxtHandle& context) {
	PCERT_CONTEXT certContext = nullptr;
	[[maybe_unused]] auto ss = QueryContextAttributesW(&context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certContext);
#ifdef _DEBUG
	if (ss != SEC_E_OK)
		_RPTWN(_CRT_WARN, L"QueryContextAttributes(SECPKG_ATTR_REMOTE_CERT_CONTEXT) error: %08X.\n", ss);
#endif
	return std::unique_ptr<CERT_CONTEXT>{ certContext };
}

void ShowCertificate() {
	if (auto const& sc = AskCmdCtrlSkt(); sc && sc->IsSSLAttached())
		if (auto certContext = getCertContext(sc->sslContext)) {
			CRYPTUI_VIEWCERTIFICATE_STRUCTW certViewInfo{ sizeof CRYPTUI_VIEWCERTIFICATE_STRUCTW, 0, CRYPTUI_DISABLE_EDITPROPERTIES | CRYPTUI_DISABLE_ADDTOSTORE, nullptr, certContext.get() };
			__pragma(warning(suppress:6387)) CryptUIDlgViewCertificateW(&certViewInfo, nullptr);
		}
}

enum class CertResult {
	Secure,
	NotSecureAccepted,
	Declined,
	Failed = Declined,
};

struct CertDialog {
	using result_t = int;
	std::unique_ptr<CERT_CONTEXT> const& certContext;
	CertDialog(std::unique_ptr<CERT_CONTEXT> const& certContext) : certContext{ certContext } {}
	void OnCommand(HWND hdlg, WORD commandId) {
		switch (commandId) {
		case IDYES:
		case IDNO:
			EndDialog(hdlg, commandId);
			break;
		case IDC_SHOWCERT:
			CRYPTUI_VIEWCERTIFICATE_STRUCTW certViewInfo{ sizeof CRYPTUI_VIEWCERTIFICATE_STRUCTW, hdlg, CRYPTUI_DISABLE_EDITPROPERTIES | CRYPTUI_DISABLE_ADDTOSTORE, nullptr, certContext.get() };
			__pragma(warning(suppress:6387)) CryptUIDlgViewCertificateW(&certViewInfo, nullptr);
			break;
		}
	}
};

static CertResult ConfirmSSLCertificate(CtxtHandle& context, wchar_t* serverName, BOOL* pbAborted) {
	auto certContext = getCertContext(context);
	if (!certContext)
		return CertResult::Failed;

	auto chainContext = [&certContext]() {
		CERT_CHAIN_PARA chainPara{ sizeof(CERT_CHAIN_PARA) };
		PCCERT_CHAIN_CONTEXT chainContext;
		auto result = CertGetCertificateChain(nullptr, certContext.get(), nullptr, nullptr, &chainPara, CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, nullptr, &chainContext);
		return std::unique_ptr<const CERT_CHAIN_CONTEXT>{ result ? chainContext : nullptr };
	}();
	if (!chainContext)
		return CertResult::Failed;
	SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslPolicy{ sizeof(SSL_EXTRA_CERT_CHAIN_POLICY_PARA), AUTHTYPE_SERVER, 0, serverName };
	CERT_CHAIN_POLICY_PARA policyPara{ sizeof(CERT_CHAIN_POLICY_PARA), 0, &sslPolicy };
	CERT_CHAIN_POLICY_STATUS policyStatus{ sizeof(CERT_CHAIN_POLICY_STATUS) };
	if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chainContext.get(), &policyPara, &policyStatus))
		return CertResult::Failed;
	if (policyStatus.dwError == 0)
		return CertResult::Secure;
	Notice(IDS_CERTERROR, policyStatus.dwError, GetErrorMessage(policyStatus.dwError));

	// thumbprint比較
	static std::vector<std::array<unsigned char, 20>> acceptedThumbprints;
	std::array<unsigned char, 20> thumbprint;
	if (auto size = size_as<DWORD>(thumbprint); !CertGetCertificateContextProperty(certContext.get(), CERT_HASH_PROP_ID, data(thumbprint), &size))
		return CertResult::Failed;
	if (std::find(begin(acceptedThumbprints), end(acceptedThumbprints), thumbprint) != end(acceptedThumbprints))
		return CertResult::NotSecureAccepted;

	if (Dialog(GetFtpInst(), certerr_dlg, GetMainHwnd(), CertDialog{ certContext }) == IDYES) {
		acceptedThumbprints.push_back(thumbprint);
		return CertResult::NotSecureAccepted;
	}
	*pbAborted = YES;
	return CertResult::Declined;
}

// SSLセッションを開始
BOOL SocketContext::AttachSSL(BOOL* pbAborted) {
	assert(SecIsValidHandle(&credential));
	auto first = true;
	SECURITY_STATUS ss = SEC_I_CONTINUE_NEEDED;
	do {
		SecBuffer inBuffer[]{ { 0, SECBUFFER_EMPTY, nullptr }, { 0, SECBUFFER_EMPTY, nullptr } };
		SecBuffer outBuffer[]{ { 0, SECBUFFER_EMPTY, nullptr }, { 0, SECBUFFER_EMPTY, nullptr } };
		SecBufferDesc inDesc{ SECBUFFER_VERSION, size_as<unsigned long>(inBuffer), inBuffer };
		SecBufferDesc outDesc{ SECBUFFER_VERSION, size_as<unsigned long>(outBuffer), outBuffer };
		unsigned long attr = 0;
		if (first) {
			first = false;
			__pragma(warning(suppress:6001)) ss = InitializeSecurityContextW(&credential, nullptr, const_cast<SEC_WCHAR*>(punyTarget.c_str()), contextReq, 0, 0, nullptr, 0, &sslContext, &outDesc, &attr, nullptr);
		} else {
			char buffer[8192];
			if (auto read = recv(handle, buffer, size_as<int>(buffer), 0); read == 0) {
				Debug(L"AttachSSL(): recv: connection closed."sv);
				return FALSE;
			} else if (0 < read) {
				_RPTWN(_CRT_WARN, L"AttachSSL recv: %d bytes.\n", read);
				readRaw.insert(end(readRaw), buffer, buffer + read);
			} else if (auto lastError = WSAGetLastError(); lastError != WSAEWOULDBLOCK) {
				Error(L"AttachSSL(): recv()"sv, lastError);
				return FALSE;
			}
			inBuffer[0] = { size_as<unsigned long>(readRaw), SECBUFFER_TOKEN, data(readRaw) };
			ss = InitializeSecurityContextW(&credential, &sslContext, const_cast<SEC_WCHAR*>(punyTarget.c_str()), contextReq, 0, 0, &inDesc, 0, nullptr, &outDesc, &attr, nullptr);
		}
		_RPTWN(_CRT_WARN, L"AttachSSL(): InitializeSecurityContextW(): in=%d, 0x%08X, in: %d/%d/%p, %d/%d/%p, out: %d/%d/%p, %d/%d/%p, attr=%X.\n", size_as<int>(readRaw), ss,
			inBuffer[0].BufferType, inBuffer[0].cbBuffer, inBuffer[0].pvBuffer, inBuffer[1].BufferType, inBuffer[1].cbBuffer, inBuffer[1].pvBuffer,
			outBuffer[0].BufferType, outBuffer[0].cbBuffer, outBuffer[0].pvBuffer, outBuffer[1].BufferType, outBuffer[1].cbBuffer, outBuffer[1].pvBuffer,
			attr
		);
		if (FAILED(ss) && ss != SEC_E_INCOMPLETE_MESSAGE) {
			Error(L"AttachSSL(): InitializeSecurityContext()"sv, ss);
			return FALSE;
		}
		if (outBuffer[0].BufferType == SECBUFFER_TOKEN && outBuffer[0].cbBuffer != 0) {
			auto written = send(handle, reinterpret_cast<const char*>(outBuffer[0].pvBuffer), outBuffer[0].cbBuffer, 0);
			assert(written == outBuffer[0].cbBuffer);
			_RPTWN(_CRT_WARN, L"AttachSSL(): send: %d bytes.\n", written);
			FreeContextBuffer(outBuffer[0].pvBuffer);
		}
		if (ss == SEC_E_INCOMPLETE_MESSAGE)
			ss = SEC_I_CONTINUE_NEEDED;
		else if (inBuffer[1].BufferType == SECBUFFER_EXTRA)
			// inBuffer[1].pvBufferはnullptrの場合があるためinBuffer[1].cbBufferのみを使用する
			readRaw.erase(begin(readRaw), end(readRaw) - inBuffer[1].cbBuffer);
		else
			readRaw.clear();
		Sleep(0);
	} while (ss == SEC_I_CONTINUE_NEEDED);

	if (ss = QueryContextAttributesW(&sslContext, SECPKG_ATTR_STREAM_SIZES, &sslStreamSizes); ss != SEC_E_OK) {
		Error(L"AttachSSL(): QueryContextAttributes(SECPKG_ATTR_STREAM_SIZES)"sv, ss);
		return FALSE;
	}

	switch (ConfirmSSLCertificate(sslContext, const_cast<wchar_t*>(punyTarget.c_str()), pbAborted)) {
	case CertResult::Secure:
		sslSecure = true;
		break;
	case CertResult::NotSecureAccepted:
		sslSecure = false;
		break;
	default:
		return FALSE;
	}
	if (!empty(readRaw))
		Decypt();
	_RPTW0(_CRT_WARN, L"AttachSSL(): success.\n");
	return TRUE;
}

bool IsSecureConnection() {
	if (auto const& sc = AskCmdCtrlSkt(); sc && sc->IsSSLAttached())
		return sc->sslSecure;
	return false;
}


int SocketContext::RecvInternal(char* buf, int len, int flags) {
	assert(flags == 0 || flags == MSG_PEEK);
	if (!IsSSLAttached())
		return recv(handle, buf, len, flags);

	if (empty(readPlain) && sslReadStatus != SEC_I_CONTEXT_EXPIRED) {
		auto offset = size_as<int>(readRaw);
		readRaw.resize((size_t)sslStreamSizes.cbHeader + sslStreamSizes.cbMaximumMessage + sslStreamSizes.cbTrailer);
		auto read = recv(handle, data(readRaw) + offset, size_as<int>(readRaw) - offset, 0);
		if (read <= 0) {
			readRaw.resize(offset);
#ifdef _DEBUG
			if (read == 0)
				Debug(L"FTPS_recv: recv(): connection closed."sv);
			else if (auto lastError = WSAGetLastError(); lastError != WSAEWOULDBLOCK)
				Error(L"FTPS_recv: recv()"sv, lastError);
#endif
			return read;
		}
		_RPTWN(_CRT_WARN, L"FTPS_recv recv: %d bytes.\n", read);
		readRaw.resize((size_t)offset + read);
		Decypt();
	}

	if (empty(readPlain))
		switch (sslReadStatus) {
		case SEC_I_CONTEXT_EXPIRED:
			return 0;
		case SEC_E_OK:
		case SEC_I_CONTINUE_NEEDED:
		case SEC_E_INCOMPLETE_MESSAGE:
			// recvできたデータが少なすぎてフレームの解析・デコードができず、復号データが得られないというエラー。
			// ブロッキングが発生するというエラーに書き換える。
			WSASetLastError(WSAEWOULDBLOCK);
			return SOCKET_ERROR;
		default:
			_RPTWN(_CRT_WARN, L"FTPS_recv readStatus: %08X.\n", sslReadStatus);
			return SOCKET_ERROR;
		}
	len = std::min(len, size_as<int>(readPlain));
	std::copy_n(begin(readPlain), len, buf);
	if ((flags & MSG_PEEK) == 0)
		readPlain.erase(begin(readPlain), begin(readPlain) + len);
	_RPTWN(_CRT_WARN, L"FTPS_recv read: %d bytes.\n", len);
	return len;
}


int MakeSocketWin() {
	auto const className = L"FFFTPSocketWnd";
	WNDCLASSEXW wcx{ sizeof(WNDCLASSEXW), 0, SocketWndProc, 0, 0, GetFtpInst(), nullptr, nullptr, CreateSolidBrush(GetSysColor(COLOR_INFOBK)), nullptr, className, };
	RegisterClassExW(&wcx);
	if (hWndSocket = CreateWindowExW(0, className, nullptr, WS_BORDER | WS_POPUP, 0, 0, 0, 0, GetMainHwnd(), nullptr, GetFtpInst(), nullptr)) {
		std::lock_guard lock{ mapMutex };
		map.clear();
		return FFFTP_SUCCESS;
	}
	return FFFTP_FAIL;
}


void DeleteSocketWin() {
	if (hWndSocket)
		DestroyWindow(hWndSocket);
}


static LRESULT CALLBACK SocketWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_ASYNC_SOCKET) {
		std::lock_guard lock{ mapMutex };
		if (auto it = map.find(wParam); it != end(map)) {
			it->second->error = WSAGETSELECTERROR(lParam);
			it->second->event |= WSAGETSELECTEVENT(lParam);
		}
		return 0;
	}
	return DefWindowProcW(hWnd, message, wParam, lParam);
}


bool SocketContext::GetEvent(int mask) {
	if ((event & mask) == 0)
		return false;
	event &= ~(mask & (FD_ACCEPT | FD_READ | FD_WRITE));
	return true;
}


std::shared_ptr<SocketContext> SocketContext::Create(int af, std::variant<std::wstring_view, std::reference_wrapper<const SocketContext>> originalTarget) {
	auto s = socket(af, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		WSAError(L"socket()"sv);
		return {};
	}
	if (originalTarget.index() == 0) {
		auto target = std::get<0>(originalTarget);
		return std::make_shared<SocketContext>(s, std::wstring{ target }, IdnToAscii(target));
	} else {
		SocketContext const& target = std::get<1>(originalTarget);
		return std::make_shared<SocketContext>(s, target.originalTarget, target.punyTarget);
	}
}


SocketContext::SocketContext(SOCKET s, std::wstring originalTarget, std::wstring punyTarget) : handle{ s }, originalTarget{ originalTarget }, punyTarget{ punyTarget } {
	std::lock_guard lock{ mapMutex };
	map[handle] = this;
}


int SocketContext::Close() {
	WSAAsyncSelect(handle, hWndSocket, WM_ASYNC_SOCKET, 0);
	{
		std::lock_guard lock{ mapMutex };
		map.erase(handle);
	}
	if (int result = closesocket(handle); result != SOCKET_ERROR)
		return result;
	for (;;) {
		if (error != 0)
			return SOCKET_ERROR;
		if (GetEvent(FD_CLOSE))
			return 0;
		Sleep(1);
		if (BackgrndMessageProc() == YES)
			return SOCKET_ERROR;
	}
}


int SocketContext::Connect(const sockaddr* name, int namelen, int* CancelCheckWork) {
	if (WSAAsyncSelect(handle, hWndSocket, WM_ASYNC_SOCKET, FD_CONNECT | FD_CLOSE | FD_ACCEPT) != 0) {
		WSAError(L"do_connect: WSAAsyncSelect()"sv);
		return SOCKET_ERROR;
	}
	if (connect(handle, name, namelen) == 0)
		return 0;
	if (auto lastError = WSAGetLastError(); lastError != WSAEWOULDBLOCK) {
		Error(L"do_connect: connect()"sv, lastError);
		return SOCKET_ERROR;
	}
	while (*CancelCheckWork != YES) {
		if (error == 0 && GetEvent(FD_CONNECT))
			return 0;
		if (error != 0 && error != WSAEWOULDBLOCK) {
			Error(L"do_connect: select()"sv, error);
			return SOCKET_ERROR;
		}
		Sleep(1);
		if (BackgrndMessageProc() == YES)
			*CancelCheckWork = YES;
	}
	return SOCKET_ERROR;
}


int SocketContext::Listen(int backlog) {
	if (WSAAsyncSelect(handle, hWndSocket, WM_ASYNC_SOCKET, FD_CLOSE | FD_ACCEPT) != 0) {
		WSAError(L"do_listen: WSAAsyncSelect()"sv);
		return SOCKET_ERROR;
	}
	if (listen(handle, backlog) != 0) {
		WSAError(L"do_listen: listen()"sv);
		return SOCKET_ERROR;
	}
	return 0;
}


std::shared_ptr<SocketContext> SocketContext::Accept(_Out_writes_bytes_opt_(*addrlen) struct sockaddr* addr, _Inout_opt_ int* addrlen) {
	for (;;) {
		if (error != 0 || GetEvent(FD_CLOSE))
			return {};
		if (GetEvent(FD_ACCEPT))
			break;
		Sleep(1);
		if (BackgrndMessageProc() == YES)
			return {};
	}
	if (error == 0) {
		do {
			auto s = accept(handle, addr, addrlen);
			if (s != INVALID_SOCKET) {
				auto sc = std::make_shared<SocketContext>(s, originalTarget, punyTarget);
				if (WSAAsyncSelect(sc->handle, hWndSocket, WM_ASYNC_SOCKET, FD_CONNECT | FD_CLOSE | FD_ACCEPT) != SOCKET_ERROR)
					return sc;
				sc->Close();
				break;
			}
			error = WSAGetLastError();
			Sleep(1);
			if (BackgrndMessageProc() == YES)
				break;
		} while (error == WSAEWOULDBLOCK);
	}
	return {};
}


int SocketContext::Recv(char* buf, int len, int flags, int* TimeOutErr, int* CancelCheckWork) {
	if (*CancelCheckWork != NO)
		return SOCKET_ERROR;
	auto endTime = TimeOut != 0 ? std::make_optional(std::chrono::steady_clock::now() + std::chrono::seconds(TimeOut)) : std::nullopt;
	*TimeOutErr = NO;
	for (;;) {
		if (auto read = RecvInternal(buf, len, flags); read != SOCKET_ERROR)
			return read;
		if (auto lastError = WSAGetLastError(); lastError != WSAEWOULDBLOCK)
			return SOCKET_ERROR;
		Sleep(1);
		if (BackgrndMessageProc() == YES)
			return SOCKET_ERROR;
		if (endTime && *endTime < std::chrono::steady_clock::now()) {
			Debug(L"do_recv timed out."sv);
			*TimeOutErr = YES;
			*CancelCheckWork = YES;
			return SOCKET_ERROR;
		}
		if (*CancelCheckWork == YES)
			return SOCKET_ERROR;
	}
}


int SocketContext::Send(const char* buf, int len, int flags, int* CancelCheckWork) {
	if (len <= 0)
		return FFFTP_SUCCESS;

	// バッファの構築、SSLの場合には暗号化を行う
	std::vector<char> work;
	std::string_view buffer{ buf, size_t(len) };
	if (IsSSLAttached()) {
		if (work = Encrypt(buffer); empty(work)) {
			Debug(L"SendData: EncryptMessage failed."sv);
			return FFFTP_FAIL;
		}
		buffer = { data(work), size(work) };
	}

	// SSLの場合には暗号化されたバッファなため、全てのデータを送信するまで繰り返す必要がある（途中で中断しても再開しようがない）
	if (*CancelCheckWork != NO)
		return FFFTP_FAIL;
	auto endTime = TimeOut != 0 ? std::optional{ std::chrono::steady_clock::now() + std::chrono::seconds(TimeOut) } : std::nullopt;
	do {
		auto sent = send(handle, data(buffer), size_as<int>(buffer), flags);
		if (0 < sent)
			buffer = buffer.substr(sent);
		else if (sent == 0) {
			Debug(L"SendData: send(): connection closed."sv);
			return FFFTP_FAIL;
		} else if (auto lastError = WSAGetLastError(); lastError != WSAEWOULDBLOCK) {
			Error(L"SendData: send()"sv, lastError);
			return FFFTP_FAIL;
		}
		Sleep(1);
		if (BackgrndMessageProc() == YES || *CancelCheckWork == YES)
			return FFFTP_FAIL;
		if (endTime && *endTime < std::chrono::steady_clock::now()) {
			Notice(IDS_MSGJPN241);
			*CancelCheckWork = YES;
			return FFFTP_FAIL;
		}
	} while (!empty(buffer));
	return FFFTP_SUCCESS;
}


void SocketContext::RemoveReceivedData() {
	char buf[1024];
	int len;
	while ((len = RecvInternal(buf, sizeof(buf), MSG_PEEK)) > 0)
		RecvInternal(buf, len, 0);
}


// UPnP対応
static ComPtr<IUPnPNAT> upnpNAT;
static ComPtr<IStaticPortMappingCollection> staticPortMappingCollection;

int LoadUPnP() {
	if (IsMainThread())
		if (CoCreateInstance(CLSID_UPnPNAT, NULL, CLSCTX_ALL, IID_PPV_ARGS(&upnpNAT)) == S_OK)
			if (upnpNAT->get_StaticPortMappingCollection(&staticPortMappingCollection) == S_OK)
				return FFFTP_SUCCESS;
	return FFFTP_FAIL;
}

void FreeUPnP() {
	if (IsMainThread()) {
		staticPortMappingCollection.Reset();
		upnpNAT.Reset();
	}
}

int IsUPnPLoaded() {
	return upnpNAT && staticPortMappingCollection ? YES : NO;
}


std::optional<std::wstring> AddPortMapping(std::wstring const& internalAddress, int port) {
	static _bstr_t TCP{ L"TCP" };
	static _bstr_t FFFTP{ L"FFFTP" };
	struct Data : public MainThreadRunner {
		long port;
		std::wstring const& internalAddress;
		_bstr_t externalAddress;
		Data(std::wstring const& internalAddress, long port) : port{ port }, internalAddress{ internalAddress } {}
		int DoWork() override {
			ComPtr<IStaticPortMapping> staticPortMapping;
			auto result = staticPortMappingCollection->Add(port, TCP, port, _bstr_t{ internalAddress.c_str() }, VARIANT_TRUE, FFFTP, &staticPortMapping);
			if (result == S_OK)
				result = staticPortMapping->get_ExternalIPAddress(externalAddress.GetAddress());
			return result;
		}
	} data{ internalAddress, port };
	if (auto result = (HRESULT)data.Run(); result == S_OK)
		return { { (const wchar_t*)data.externalAddress, data.externalAddress.length() } };
	return {};
}

bool RemovePortMapping(int port) {
	static _bstr_t TCP{ L"TCP" };
	struct Data : public MainThreadRunner {
		long port;
		Data(long port) : port{ port } {}
		int DoWork() override {
			return staticPortMappingCollection->Remove(port, TCP);
		}
	} data{ port };
	auto result = (HRESULT)data.Run();
	return result == S_OK;
}


int CheckClosedAndReconnect() {
	if (auto const& sc = AskCmdCtrlSkt(); !sc || sc->error != 0 || sc->GetEvent(FD_CLOSE))
		return ReConnectCmdSkt();
	return FFFTP_SUCCESS;
}


int CheckClosedAndReconnectTrnSkt(std::shared_ptr<SocketContext>& Skt, int* CancelCheckWork) {
	if (!Skt || Skt->error != 0 || Skt->GetEvent(FD_CLOSE))
		return ReConnectTrnSkt(Skt, CancelCheckWork);
	return FFFTP_SUCCESS;
}

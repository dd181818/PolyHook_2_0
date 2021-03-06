#include "headers/PE/EatHook.hpp"

PLH::EatHook::EatHook(const std::string& apiName, const std::wstring& moduleName, const char* fnCallback, uint64_t* userOrigVar)
	: EatHook(apiName, moduleName, (uint64_t)fnCallback, userOrigVar)
{}

PLH::EatHook::EatHook(const std::string& apiName, const std::wstring& moduleName, const uint64_t fnCallback, uint64_t* userOrigVar)
	: m_apiName(apiName)
	, m_moduleName(moduleName)
    , m_userOrigVar(userOrigVar)
    , m_fnCallback(fnCallback)
{}

bool PLH::EatHook::hook() {
	assert(m_userOrigVar != nullptr);
	uint32_t* pExport = FindEatFunction(m_apiName, m_moduleName);
	if (pExport == nullptr)
		return false;

	// Just like IAT, EAT is by default a writeable section
	// any EAT entry must be an offset
	MemoryProtector prot((uint64_t)pExport, sizeof(uintptr_t), ProtFlag::R | ProtFlag::W);
	m_origFunc = *pExport;
	*pExport = (uint32_t)(m_fnCallback - m_moduleBase);
	m_hooked = true;
	*m_userOrigVar = m_origFunc;
	return true;
}

bool PLH::EatHook::unHook() {
	assert(m_userOrigVar != nullptr);
	assert(m_hooked);
	if (!m_hooked)
		return false;

	uint32_t* pExport = FindEatFunction(m_apiName, m_moduleName);
	if (pExport == nullptr)
		return false;

	MemoryProtector prot((uint64_t)pExport, sizeof(uintptr_t), ProtFlag::R | ProtFlag::W);
	*pExport = (uint32_t)m_origFunc;
	m_hooked = false;
	*m_userOrigVar = NULL;
	return true;
}

uint32_t* PLH::EatHook::FindEatFunction(const std::string& apiName, const std::wstring& moduleName) {
#if defined(_WIN64)
	PEB* peb = (PPEB)__readgsqword(0x60);
#else
	PEB* peb = (PPEB)__readfsdword(0x30);
#endif

	uint32_t* pExportAddress = nullptr;
	PEB_LDR_DATA* ldr = (PPEB_LDR_DATA)peb->Ldr;

	// find loaded module from peb
	for (LDR_DATA_TABLE_ENTRY* dte = (LDR_DATA_TABLE_ENTRY*)ldr->InLoadOrderModuleList.Flink;
		 dte->DllBase != NULL;
		 dte = (LDR_DATA_TABLE_ENTRY*)dte->InLoadOrderLinks.Flink) {

		// TODO: create stricmp for UNICODE_STRING because this is really bad for performance
		std::wstring baseModuleName(dte->BaseDllName.Buffer, dte->BaseDllName.Length / sizeof(wchar_t));

		// try all modules if none given, otherwise only try specified
		if (!moduleName.empty() && (my_wide_stricmp(baseModuleName.c_str(), moduleName.c_str()) != 0))
			continue;

		m_moduleBase = (uint64_t)dte->DllBase;

		pExportAddress = FindEatFunctionInModule(apiName);
		if (pExportAddress != nullptr)
			return pExportAddress;
	}

	if (pExportAddress == nullptr) {
		ErrorLog::singleton().push("Failed to find export address from requested dll", ErrorLevel::SEV);
	}
	return pExportAddress;
}

uint32_t* PLH::EatHook::FindEatFunctionInModule(const std::string& apiName) {
	assert(m_moduleBase != NULL);
	if (m_moduleBase == NULL)
		return NULL;

	IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)m_moduleBase;
	IMAGE_NT_HEADERS* pNT = RVA2VA(IMAGE_NT_HEADERS*, m_moduleBase, pDos->e_lfanew);
	IMAGE_DATA_DIRECTORY* pDataDir = (IMAGE_DATA_DIRECTORY*)pNT->OptionalHeader.DataDirectory;

	if (pDataDir[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == NULL) {
		ErrorLog::singleton().push("PEs without export tables are unsupported", ErrorLevel::SEV);
		return NULL;
	}

	IMAGE_EXPORT_DIRECTORY* pExports = RVA2VA(IMAGE_EXPORT_DIRECTORY*, m_moduleBase, pDataDir[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

	uint32_t* pAddressOfFunctions = RVA2VA(uint32_t*, m_moduleBase, pExports->AddressOfFunctions);
	uint32_t* pAddressOfNames = RVA2VA(uint32_t*, m_moduleBase, pExports->AddressOfNames);
	uint16_t* pAddressOfNameOrdinals = RVA2VA(uint16_t*, m_moduleBase, pExports->AddressOfNameOrdinals);

	for (uint32_t i = 0; i < pExports->NumberOfFunctions; i++)
	{	
        if(my_narrow_stricmp(RVA2VA(char*, m_moduleBase, pAddressOfNames[i]),
                             apiName.c_str()) != 0)
			continue;	 				

		uint16_t iExportOrdinal = RVA2VA(uint16_t, m_moduleBase, pAddressOfNameOrdinals[i]);
		uint32_t* pExportAddress = &pAddressOfFunctions[iExportOrdinal];

		return pExportAddress;
	}

	ErrorLog::singleton().push("API not found before end of EAT", ErrorLevel::SEV);
	return nullptr;
}
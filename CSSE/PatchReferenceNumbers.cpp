#include "PatchReferenceNumbers.h"

#include "LogUtil.h"
#include "MemoryUtil.h"
#include "PathUtil.h"
#include "Settings.h"
#include "StringUtil.h"

#include "CSCell.h"
#include "CSGameFile.h"
#include "CSGlobalVariable.h"
#include "CSRecordHandler.h"
#include "CSReference.h"

namespace se::cs::patch::reference_numbers {
	constexpr DWORD ModMask = 0xFF000000;
	constexpr DWORD MaxReferenceNumber = 0x00FFFFFF;
	constexpr auto MaxRefIndexGlobal = "MWSE_MAX_REF_INDEX";

	struct FileState {
		DWORD highest = 0;
		std::unordered_set<DWORD> writtenThisSave;
		std::unordered_map<const Reference*, DWORD> reserved;
	};

	std::unordered_map<std::string, FileState> fileStates;

	std::string getFileKey(const GameFile& file) {
		std::string key = file.fileName;
		string::to_lower(key);
		return key;
	}

	FileState& getState(const GameFile& file) {
		return fileStates[getFileKey(file)];
	}

	bool hasValidNumber(const Reference& reference) {
		const auto number = static_cast<DWORD>(reference.targetID);
		return number != 0 && (number & ModMask) == 0;
	}

	// New references don't get a source file until they are saved.
	bool isReferenceOwnedByFile(const Reference& reference, const GameFile& file) {
		return !reference.isFromMaster() && (reference.sourceFile == &file || reference.sourceFile == nullptr);
	}

	DWORD getNewReferenceNumber(GameFile& file, FileState& state) {
		const DWORD number = std::max(file.lastReferenceNumber, state.highest) + 1;
		file.lastReferenceNumber = number;
		state.highest = number;
		return number;
	}

	template <typename Func>
	void forEachReference(const RecordHandler& recordHandler, Func&& func) {
		for (auto cell : *recordHandler.cells) {
			if (cell == nullptr) {
				continue;
			}
			for (auto list : { &cell->cellNpcRefs, &cell->cellObjRefs }) {
				for (auto reference : *list) {
					if (reference) {
						func(*reference);
					}
				}
			}
		}
	}

	//
	// MWSE_MAX_REF_INDEX is read straight from the active plugin, so other plugins' copies don't matter.
	//

	std::optional<DWORD> readMaxRefIndex(const std::filesystem::path& path) {
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) {
			return {};
		}

		struct RecordHeader {
			char tag[4];
			std::uint32_t size;
			std::uint32_t unknown;
			std::uint32_t flags;
		};
		static_assert(sizeof(RecordHeader) == 16);

		RecordHeader header = {};
		while (file.read(reinterpret_cast<char*>(&header), sizeof(header))) {
			if (std::memcmp(header.tag, "GLOB", 4) != 0) {
				file.seekg(header.size, std::ios::cur);
				continue;
			}

			std::vector<char> data(header.size);
			if (!file.read(data.data(), data.size())) {
				break;
			}

			std::string_view name;
			std::optional<float> value;
			for (size_t i = 0; i + 8 <= data.size();) {
				std::uint32_t size = 0;
				std::memcpy(&size, &data[i + 4], sizeof(size));
				const auto payload = i + 8;
				if (payload + size > data.size()) {
					break;
				}

				if (std::memcmp(&data[i], "NAME", 4) == 0) {
					name = std::string_view(&data[payload], strnlen(&data[payload], size));
				}
				else if (std::memcmp(&data[i], "FLTV", 4) == 0 && size == sizeof(float)) {
					float f = 0.0f;
					std::memcpy(&f, &data[payload], sizeof(f));
					value = f;
				}
				i = payload + size;
			}

			if (string::iequal(name, MaxRefIndexGlobal) && value && *value >= 0.0f && *value <= float(MaxReferenceNumber)) {
				return static_cast<DWORD>(*value);
			}
		}

		return {};
	}

	void writeMaxRefIndex(RecordHandler& recordHandler, GameFile& file, DWORD highest) {
		if (highest == 0) {
			return;
		}

		auto global = recordHandler.getGlobal(MaxRefIndexGlobal);
		if (global == nullptr) {
			global = new GlobalVariable(MaxRefIndexGlobal);
			global->valueType = 'l';
			global->sourceFile = &file;
			recordHandler.globals->push_back(global);
		}

		global->value = static_cast<float>(highest);
		global->setModified(true);
	}

	//
	// Hooks.
	//

	void __cdecl OnLoadReference(Reference* reference, GameFile* file, DWORD formId) {
		reference->sourceID = 0;
		reference->targetID = 0;

		if (formId == 0 || (formId & ModMask) != 0) {
			return;
		}

		reference->targetID = static_cast<int>(formId);

		auto& state = getState(*file);
		state.highest = std::max(state.highest, formId);
	}

	// Number all of the plugin's references before anything is written, so MWSE_MAX_REF_INDEX is final when globals are saved.
	void __cdecl OnBeforeSaveGlobals(RecordHandler* recordHandler, GameFile* file) {
		auto& state = getState(*file);
		state.writtenThisSave.clear();
		state.reserved.clear();

		std::unordered_set<DWORD> used;
		std::vector<Reference*> needNumbers;
		size_t duplicates = 0;

		forEachReference(*recordHandler, [&](Reference& reference) {
			if (isReferenceOwnedByFile(reference, *file)) {
				if (!hasValidNumber(reference)) {
					needNumbers.push_back(&reference);
				}
				else if (!used.insert(static_cast<DWORD>(reference.targetID)).second) {
					needNumbers.push_back(&reference);
					duplicates++;
				}
				else {
					state.highest = std::max(state.highest, static_cast<DWORD>(reference.targetID));
				}
			}
			else if (!reference.isFromMaster() && reference.getModified()) {
				// Modified references from other plugins get a new number, as in vanilla.
				state.reserved[&reference] = 0;
			}
		});

		for (auto reference : needNumbers) {
			reference->targetID = static_cast<int>(getNewReferenceNumber(*file, state));
		}
		for (auto& [reference, number] : state.reserved) {
			number = getNewReferenceNumber(*file, state);
		}

		writeMaxRefIndex(*recordHandler, *file, state.highest);

		log::stream << "[ReferenceNumbers] Saving " << file->fileName << ": " << used.size() << " references kept their number, "
			<< needNumbers.size() << " new numbers assigned (" << duplicates << " duplicates). " << MaxRefIndexGlobal << " = " << state.highest << "." << std::endl;
	}

	DWORD __cdecl OnSaveReferenceNumber(Reference* reference, GameFile* file, DWORD current, bool vanillaWantsNew) {
		auto& state = getState(*file);

		if (isReferenceOwnedByFile(*reference, *file)) {
			if (hasValidNumber(*reference) && state.writtenThisSave.insert(static_cast<DWORD>(reference->targetID)).second) {
				return static_cast<DWORD>(reference->targetID);
			}
			reference->targetID = static_cast<int>(getNewReferenceNumber(*file, state));
			state.writtenThisSave.insert(static_cast<DWORD>(reference->targetID));
			log::stream << "[ReferenceNumbers] Late number " << reference->targetID << " for '" << reference->getObjectID() << "'; " << MaxRefIndexGlobal << " is now out of date." << std::endl;
			return static_cast<DWORD>(reference->targetID);
		}

		if (reference->isFromMaster() && !vanillaWantsNew && current != 0) {
			return current;
		}

		if (const auto reserved = state.reserved.find(reference); reserved != state.reserved.end()) {
			return reserved->second;
		}

		return getNewReferenceNumber(*file, state);
	}

	// Replaces 0x53657A. esi = reference, edi = file, [esp+18h] = FRMR.
	__declspec(naked) void PatchLoadReference() {
		__asm {
			mov eax, [esp + 0x18]
			push eax
			push edi
			push esi
			call OnLoadReference
			add esp, 0xC
			xor eax, eax
			push eax
			mov ecx, 0x536583
			jmp ecx
		}
	}

	// Replaces 0x53877D. ebp = reference, esi = file, [esp+10h] = FRMR.
	__declspec(naked) void PatchSaveReferenceNumber() {
		__asm {
			mov eax, [esp + 0x10]
			push 0
			push eax
			push esi
			push ebp
			call OnSaveReferenceNumber
			add esp, 0x10
			mov [esp + 0x10], eax
			mov eax, 0x538796
			jmp eax
		}
	}

	// Replaces 0x538785, the path where vanilla always assigns a new number.
	__declspec(naked) void PatchSaveNewReferenceNumber() {
		__asm {
			mov eax, [esp + 0x10]
			push 1
			push eax
			push esi
			push ebp
			call OnSaveReferenceNumber
			add esp, 0x10
			mov [esp + 0x10], eax
			mov eax, 0x538796
			jmp eax
		}
	}

	// Replaces 0x502295, right before globals are saved. esi = record handler, ebp = file.
	__declspec(naked) void PatchBeforeSaveGlobals() {
		__asm {
			push ebp
			push esi
			call OnBeforeSaveGlobals
			add esp, 0x8
			push 0x6BD994
			mov eax, 0x402BFD
			call eax
			mov eax, 0x50229F
			jmp eax
		}
	}

	bool bytesMatch(DWORD address, std::initializer_list<BYTE> expected) {
		auto current = reinterpret_cast<const BYTE*>(address);
		for (const auto b : expected) {
			if (*current++ != b) {
				return false;
			}
		}
		return true;
	}

	bool installed = false;

	void onFilesLoaded(RecordHandler& recordHandler) {
		if (!installed || recordHandler.activeFile == nullptr) {
			return;
		}

		auto& file = *recordHandler.activeFile;
		if (const auto value = readMaxRefIndex(path::getDataFilesPath() / file.fileName)) {
			auto& state = getState(file);
			state.highest = std::max(state.highest, *value);
		}
	}

	void installPatches() {
		if (!settings.reference_numbers.preserve) {
			return;
		}

		const bool valid = bytesMatch(0x53657A, { 0x33, 0xC0, 0x89, 0x46, 0x70, 0x89, 0x46, 0x6C, 0x50 })
			&& bytesMatch(0x53877D, { 0x8B, 0x44, 0x24, 0x10, 0x85, 0xC0, 0x75, 0x11 })
			&& bytesMatch(0x538785, { 0x8B, 0x86, 0xEC, 0x04, 0x00, 0x00, 0x40, 0x89, 0x86, 0xEC, 0x04, 0x00, 0x00, 0x89, 0x44, 0x24, 0x10 })
			&& bytesMatch(0x502295, { 0x68, 0x94, 0xD9, 0x6B, 0x00, 0xE8, 0x5E, 0x09, 0xF0, 0xFF });
		if (!valid) {
			log::stream << "[ReferenceNumbers] Unexpected code in the Construction Set executable. Patch not installed." << std::endl;
			return;
		}

		using memory::genJumpUnprotected;

		genJumpUnprotected(0x53657A, reinterpret_cast<DWORD>(PatchLoadReference), 0x9);
		genJumpUnprotected(0x53877D, reinterpret_cast<DWORD>(PatchSaveReferenceNumber), 0x8);
		genJumpUnprotected(0x538785, reinterpret_cast<DWORD>(PatchSaveNewReferenceNumber), 0x11);
		genJumpUnprotected(0x502295, reinterpret_cast<DWORD>(PatchBeforeSaveGlobals), 0xA);

		installed = true;
	}
}

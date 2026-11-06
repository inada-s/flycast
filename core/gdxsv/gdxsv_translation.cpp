//
//  gdxsv_translation.cpp
//  gdxsv
//
//  Created by Edward Li on 15/7/2021.
//  Copyright © 2021 flycast. All rights reserved.
//
#ifdef _WIN32
#define _AMD64_	 // Fixing GitHub runner's winnt.h error
#endif

#include "gdxsv_translation.h"
#include "gdxsv_config.h"

#include "cfg/option.h"
#include "oslib/i18n.h"
#include "oslib/resources.h"
#include "tinygettext/tinygettext.hpp"
#include "tinygettext/file_system.hpp"
#include <sstream>

using namespace tinygettext;

namespace {
class GdxsvResourceFileSystem : public FileSystem
{
	std::vector<std::string> open_directory(const std::string& pathname) override {
		return resource::listDirectory(pathname);
	}
	std::unique_ptr<std::istream> open_file(const std::string& filename) override
	{
		size_t size;
		std::unique_ptr<u8[]> data = resource::load(filename, size);
		if (data == nullptr)
			return nullptr;
		std::string str((const char *)&data[0], (const char *)&data[size]);
		return std::make_unique<std::istringstream>(str);
	}
};

static DictionaryManager* gdxDictMgr = nullptr;

static void initGdxDictMgr() {
	if (gdxDictMgr == nullptr) {
		gdxDictMgr = new DictionaryManager(std::make_unique<GdxsvResourceFileSystem>());
		gdxDictMgr->add_directory("i18n-gdxsv");
	}
}
}

const char* GdxsvTranslation::Text() const {
	switch (GdxsvLanguage::Language()) {
		case GdxsvLanguage::Lang::English:
			return english;
		case GdxsvLanguage::Lang::Cantonese:
			return cantonese;
		case GdxsvLanguage::Lang::Japanese:
			return japanese ? japanese : original;
	}
	return original;
}

GdxsvLanguage::Lang GdxsvLanguage::Language() {
	Lang lang = static_cast<Lang>(config::GdxLanguage.get());
	switch (lang) {
		case Lang::Japanese:
		case Lang::Cantonese:
		case Lang::English:
		case Lang::Disabled:
			return lang;
		case Lang::NOT_SET:
		default:
			lang = LanguageFromOS();
			config::GdxLanguage.set((int)lang);
			return lang;
	}
}

std::string GdxsvLanguage::TextureDirectoryName() {
	switch (Language()) {
		case Lang::English:
			return "English";
		case Lang::Cantonese:
			return "Cantonese";
		default:
			return "Japanese";
	}
}

const char* GdxsvLanguage::gdxT(const char* msg) {
	initGdxDictMgr();
	const char* name;
	switch (Language()) {
		case Lang::Japanese:
			name = "ja";
			break;
		case Lang::Cantonese:
			name = "zh_HK";
			break;
		case Lang::English:
			name = "en";
			break;
		case Lang::Disabled:
		default:
			return i18n::T(msg);
	}
	// Without a translation, translate() returns the string it was given, which dies on return: msg itself then.
	const std::string smsg(msg);
	const std::string& tr = gdxDictMgr->get_dictionary(Language::from_name(name)).translate(smsg);
	return &tr == &smsg ? msg : tr.c_str();
}

static const char* ms_names_array[] = {
	"Gundam",
	"Guncannon",
	"GM",
	"Old Zaku",
	"Zaku",
	"Char's Zaku",
	"Gouf",
	"Dom",
	"Rick Dom",
	"Gelgoog",
	"Char's Gelgoog",
	"Gyan",
	"Gogg",
	"Acguy",
	"Z'Gok",
	"Char's Z'Gok",
	"Zock",
	"Guntank",
	"Zeong",
	"Ground Type Gundam",
	"Ground Type GM",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"Elmeth", //33
	"Ball", //34
	"Braw Bro", //35
	"Grublo", //36
	"Zakrello", //37
	"Bigro", //38
	"Big Zam", //39
	"Adzam", //40
	"G-Fighter", //41
};

const char* GdxsvLanguage::GetMSName(int id) {
	if (id < 0 || id >= GetMSCount()) return "?";
	const char* name = ms_names_array[id];
	if (strlen(name) == 0) return "";
	return gdxT(name);
}

int GdxsvLanguage::GetMSCount() {
	return (int)(sizeof(ms_names_array) / sizeof(ms_names_array[0]));
}

#ifdef _WIN32
#include <winnls.h>

#include <codecvt>
#include <locale>
#endif

GdxsvLanguage::Lang GdxsvLanguage::LanguageFromOS() {
	std::string locale = i18n::getCurrentLocale();
#ifdef __APPLE__
	time_t ts = 0;
	struct tm t;
	char buf[16];
	localtime_r(&ts, &t);
	strftime(buf, sizeof(buf), "%z%Z", &t);
#endif

	if (locale.find("ja") == 0
#ifdef __APPLE__
		|| strcmp(buf, "+0900JST") == 0
#elif _WIN32
		|| strcmp(_tzname[0], "Tokyo Standard Time") == 0
#endif
	)
		return Lang::Japanese;
	else if (locale.find("yue") == 0 || locale.find("zh") == 0	// Chinese fallback
#ifdef __APPLE__
			 || strcmp(buf, "+0800HKT") == 0  // Cantonese users love using English OS
			 || strcmp(buf, "+0800CST") == 0
#elif _WIN32
			 || strcmp(_tzname[0], "China Standard Time") == 0 || strcmp(_tzname[0], "Taipei Standard Time") == 0
#endif
	)
		return Lang::Cantonese;
	else
		return Lang::English;
}

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <regex>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <curl/curl.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include "json.hpp"  // nlohmann JSON
#include <taglib/opusfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/taglib.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
namespace fs = std::filesystem;
using namespace std;
using json = nlohmann::json;
// -------------------- File & LRC Helpers --------------------
std::string read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}






bool hasEmbeddedLRC(const std::string &opusPath) {
    TagLib::Ogg::Opus::File file(opusPath.c_str());
    if (!file.isOpen()) return false;

    TagLib::Ogg::XiphComment *tag = file.tag();
    if (!tag) return false;

    auto fields = tag->fieldListMap(); // Get all fields
    if (fields.contains("LYRICS") || fields.contains("UNSYNCEDLYRICS")) {
        // fieldListMap returns TagLib::StringList
        TagLib::StringList lyrics = fields["LYRICS"];
        if (!lyrics.isEmpty()) return true;
    }

    return false;
}

namespace fs = std::filesystem;

// ---------------- Split Artists Helper ----------------
std::vector<std::string> splitArtists(const std::string &input) {
    std::vector<std::string> result;
    std::string temp = input;
    for (char &c : temp) if (c == ',') c = ';';  // normalize separators
    std::stringstream ss(temp);
    std::string item;
    while (std::getline(ss, item, ';')) {
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos)
            result.push_back(item.substr(start, end - start + 1));
    }
    return result;
}

// ---------------- Fix ARTISTS Tag ----------------


namespace fs = std::filesystem;
void fix_tags(const std::filesystem::path &path) {

    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.file()) return;

    TagLib::PropertyMap tags = f.file()->properties();

    // ----------------------------
    // FIX ARTIST / ARTISTS
    // ----------------------------
    TagLib::StringList oldValues;

    if (tags.contains("ARTISTS"))
        oldValues = tags["ARTISTS"];
    else if (tags.contains("ARTIST"))
        oldValues = tags["ARTIST"];

    if (!oldValues.isEmpty()) {

        TagLib::StringList newValues;

        for (auto &v : oldValues) {
            std::string s = v.toCString(true);

            // replace commas → semicolons
            for (char &c : s)
                if (c == ',') c = ';';

            // split
            std::stringstream ss(s);
            std::string item;

            while (std::getline(ss, item, ';')) {
                size_t a = item.find_first_not_of(" \t");
                size_t b = item.find_last_not_of(" \t");
                if (a != std::string::npos)
                    newValues.append(TagLib::String(item.substr(a, b - a + 1)));
            }
        }

        // Remove old keys
        tags.erase("ARTIST");
        tags.erase("ARTISTS");

        // Insert cleaned
        if (!newValues.isEmpty())
            tags.insert("ARTISTS", newValues);
    }

    // ----------------------------
    // FIX ALBUMARTIST (OPUS too!)
    // ----------------------------
    TagLib::StringList aa;

    if (tags.contains("ALBUMARTIST"))
        aa = tags["ALBUMARTIST"];

    if (aa.isEmpty() ||
        aa.toString().isEmpty() ||
        aa.toString() == "Unknown" ||
        aa.toString() == "unknown")
    {
        tags.erase("ALBUMARTIST");
        tags.insert("ALBUMARTIST", TagLib::StringList("Unknown"));
    }

    // APPLY & SAVE
    f.file()->setProperties(tags);
    f.file()->save();
}









void embed(const std::string &opus_path_str) {
    
    fs::path opus_file(opus_path_str);
    fs::path lrc_file = opus_file;
    lrc_file.replace_extension(".lrc");

    std::string lrcText = read_file(lrc_file);

    TagLib::Ogg::Opus::File f(opus_file.c_str());
    auto *tag = f.tag();
    tag->addField("LYRICS", TagLib::String(lrcText, TagLib::String::UTF8), true);
    f.save();
}

// -------------------- Metadata & Download --------------------
 // nlohmann::json

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std;

// ---------------- Shell Escape ----------------
std::string shell_escape(const std::string &s) {
    std::string escaped = "'";
    for (char c : s) {
        if (c == '\'') escaped += "'\\''"; // escape single quote
        else escaped += c;
    }
    escaped += "'";
    return escaped;
}

// ---------------- Metadata Struct ----------------
struct Metadata {
    std::string title;
    std::string artist;
    int duration;
    std::string thumbnail;
    int count;
};

// ---------------- getMetadata Function ----------------
Metadata getMetadata(const std::string &output_folder,
                     const std::string &track,
                     const std::string &url,
                     int count) {
    // Build path for the output .opus file
    std::string save = output_folder + "/" + track + "/" + track + ".opus";
    fs::create_directories(output_folder + "/" + track);
    int ret=0;
    // yt-dlp command with shell escaping
    std::string command =
        "yt-dlp -f bestaudio[ext=webm][acodec=opus] "
        "--extract-audio --audio-format opus --audio-quality 0 "
        "--embed-metadata --no-embed-thumbnail --write-info-json "
        "-o " + shell_escape(save) + " " + shell_escape(url);

    std::cout << "Running command:\n" << command << "\n";
    if(fs::exists(save)==0){
        ret = std::system(command.c_str());
        fix_tags(save);
    };
    
    
    if(ret!=0 && count!=0){
        
        count--;
        return  getMetadata(output_folder,track,url,count);
    };
    
    if(fs::exists(output_folder+"/"+track+"/"+track+".lrc")){
        return {"Unknown", "Unknown", -1, "ukg", count};
    };
    // Find .info.json file
    fs::path json_file;
    for (auto &p : fs::directory_iterator(output_folder + "/" + track)) {
        if (p.path().extension() == ".json" &&
            p.path().string().find(".info.json") != std::string::npos) {
            json_file = p.path();
            break;
        }
    }

    if (json_file.empty()) {
        std::cerr << "JSON not found!\n";
        return {"Unknown", "Unknown", -1, "try",count};
    }

    std::ifstream ifs(json_file);
    if (!ifs.is_open()) {
        std::cerr << "Failed to open JSON file!\n";
        return {"Unknown", "Unknown", -1, ""};
    }

    json j;
    try { ifs >> j; } catch (...) { return {"Unknown", "Unknown", 0, ""}; }

    return {
        j.value("title", "Unknown"),
        j.value("artist", "Unknown"),
        j.value("duration", 0),
        j.value("thumbnail", ""),
        count
    };
}

// ---------------- Main ----------------


// -------------------- Cover Download --------------------
size_t writeData(void* ptr, size_t size, size_t nmemb, std::vector<uint8_t> *data) {
    uint8_t* p = static_cast<uint8_t*>(ptr);
    data->insert(data->end(), p, p + size * nmemb);
    return size * nmemb;
}

bool downloadCover(const std::string &url, const std::string &outputFile) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::vector<uint8_t> webpData;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &webpData);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (curl_easy_perform(curl) != CURLE_OK) { 
        curl_easy_cleanup(curl); 
        return false; 
    }
    curl_easy_cleanup(curl);

    int width, height;
    uint8_t* image = WebPDecodeRGBA(webpData.data(), webpData.size(), &width, &height);
    if (!image) return false;

    int squareSize = std::min(width, height);
    int offsetX = (width - squareSize) / 2;
    int offsetY = (height - squareSize) / 2;
    std::vector<uint8_t> cropped(squareSize * squareSize * 4);

    for (int y = 0; y < squareSize; ++y) {
        for (int x = 0; x < squareSize; ++x) {
            for (int c = 0; c < 4; ++c)
                cropped[4 * (y * squareSize + x) + c] = 
                    image[4 * ((y + offsetY) * width + (x + offsetX)) + c];
        }
    }

    WebPFree(image);
    uint8_t* output = nullptr;
    size_t output_size = WebPEncodeRGBA(cropped.data(), squareSize, squareSize, squareSize * 4, 90.0f, &output);
    if (output_size == 0) return false;

    std::ofstream ofs(outputFile, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(output), output_size);
    ofs.close();
    WebPFree(output);

    return true;
}

// -------------------- Lyrics Fetch --------------------
size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *output) {
    size_t total = size * nmemb;
    output->append((char*)contents, total);
    return total;
}

std::string search_lrc(const std::string &track, const std::string &artist) {
    CURL *curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    std::string query = track;
    if (!artist.empty()) query += " " + artist;

    char *encoded_query = curl_easy_escape(curl, query.c_str(), 0);
    std::string url = "https://lrclib.net/api/search?q=" + std::string(encoded_query);
    curl_free(encoded_query);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return "";

    return response;
}

vector<string> reduce_text(const string &text, int min_words = 1) {
    vector<string> reduced;
    stringstream ss(text);
    vector<string> words;
    string word;
    while (ss >> word) words.push_back(word);

    for (int i = (int)words.size() - 1; i >= min_words; --i) {
        string join;
        for (int j = 0; j < i; ++j) {
            join += words[j];
            if (j < i - 1) join += " ";
        }
        reduced.push_back(join);
    }
    return reduced;
}

bool fetch_lrc(const string &folder, const string &filename, const string &track, const string &artist, int duration) {
    if (duration <= 0) return 0;

    string lrc_path = folder + "/" + filename + ".lrc";
    ifstream infile(lrc_path);
    if (infile.good()) return 0;

    bool saved = false;
    int tolerance = 3;

    vector<string> artist_variants = {artist};
    auto reduced_artists = reduce_text(artist, 1);
    artist_variants.insert(artist_variants.end(), reduced_artists.begin(), reduced_artists.end());

    for (auto &a : artist_variants) {
        string json_str = search_lrc(track, a);
        if (json_str.empty()) continue;

        try {
            auto results = json::parse(json_str);
            for (auto &r : results) {
                if (!r.contains("syncedLyrics")) continue;
                string synced = r["syncedLyrics"];
                int api_dur = r.value("duration", 0);
                if (!synced.empty() && abs(api_dur - duration) <= tolerance) {
                    ofstream out(lrc_path);
                    out << synced;
                    out.close();
                    saved = true;
                    break;
                }
            }
        } catch (...) {}
        if (saved) break;
        this_thread::sleep_for(chrono::milliseconds(80));
    }

    if (!saved) {
        vector<string> track_variants = {track};
        auto reduced_tracks = reduce_text(track, 2);
        track_variants.insert(track_variants.end(), reduced_tracks.begin(), reduced_tracks.end());

        for (auto &t : track_variants) {
            string json_str = search_lrc(t, "");
            if (json_str.empty()) continue;

            try {
                auto results = json::parse(json_str);
                for (auto &r : results) {
                    if (!r.contains("syncedLyrics")) continue;
                    string synced = r["syncedLyrics"];
                    int api_dur = r.value("duration", 0);
                    if (!synced.empty() && abs(api_dur - duration) <= tolerance) {
                        ofstream out(lrc_path);
                        out << synced;
                        out.close();
                        saved = true;
                        break;
                    }
                }
            } catch (...) {}
            if (saved) break;
            this_thread::sleep_for(chrono::milliseconds(50));
        }
    }

    return saved;
}

// -------------------- YouTube Thumbnail --------------------
string extractVideoID(const string &url) {
    string id;
    size_t pos = string::npos;
    if ((pos = url.find("youtu.be/")) != string::npos) {
        pos += 9;
        id = url.substr(pos, 11);
    } else if ((pos = url.find("v=")) != string::npos) {
        pos += 2;
        id = url.substr(pos, 11);
    } else if ((pos = url.find("embed/")) != string::npos) {
        pos += 6;
        id = url.substr(pos, 11);
    }
    if (id.size() > 11) id = id.substr(0, 11);
    return id;
}

string youtubeToThumbnail(const string &url) {
    string id = extractVideoID(url);
    if (id.empty()) return "";
    return "https://i.ytimg.com/vi_webp/" + id + "/maxresdefault.webp";
}

// -------------------- Main Track Processor --------------------
void processTrack(string folder, string track, string url) {
    //if(fs::exists(folder+"/"+track+"/"+"cover.webp")){
        //return;
   // };
    Metadata meta = getMetadata(folder, track, url ,2);
    string title = meta.title, artist = meta.artist, thumbnail = meta.thumbnail;
    int duration = meta.duration;

    string savepath = folder + "/" + track + "/cover.webp";

    cout << "\ntitle=" << title
         << "\nartist=" << artist
         << "\nduration=" << duration
         << "\nurl=" << meta.thumbnail << endl;

    bool is_cover = downloadCover(thumbnail, savepath);

    if (!is_cover) {
        thumbnail = youtubeToThumbnail(url);
        downloadCover(thumbnail, savepath);
    }

    if (is_cover) cout << "cover found" << endl;

    bool is_embed = false;
    if (duration != 0 || duration == 1){
        is_embed = fetch_lrc(folder + "/" + track, track,title, artist, duration);
    
    };
    
    is_embed=!hasEmbeddedLRC(folder + "/" + track + "/" + track + ".opus") && fs::exists(folder + "/" + track + "/" + track + ".opus");
    
    
    
    

    if (is_embed){
        embed(folder + "/" + track + "/" + track + ".opus");
        cout<<"lyrics embedded\n\n";
    };
}

// -------------------- Multithread Worker --------------------
void worker(const string &baseDir, vector<pair<string, string>>::iterator start, vector<pair<string, string>>::iterator end) {
    for (auto it = start; it != end; ++it) {
        processTrack(baseDir, it->first, it->second);
    }
}

// -------------------- Main --------------------
int main() {
    string folder;
    cout << "📁 Enter folder name: ";
    getline(cin, folder);

    string baseDir = "/storage/emulated/0/music/" + folder;
    fs::create_directories(baseDir);

    string urlFile = baseDir + "/url.txt";
    if (!fs::exists(urlFile)) {
        cerr << "❌ url.txt not found\n";
        return 1;
    }

    ifstream infile(urlFile);
    if (!infile) {
        cerr << "❌ Cannot open " << urlFile << endl;
        return 1;
    }

    vector<pair<string, string>> tasks;
    string line;
    while (getline(infile, line)) {
        if (line.empty()) continue;
        size_t sep = line.find(">");
        if (sep == string::npos) continue;

        string title = line.substr(0, sep);
        replace(title.begin(), title.end(), ' ', '_');
        string url = line.substr(sep + 1);

        tasks.emplace_back(title, url);
    }
    infile.close();

    int n;
    cout << "🧵 Enter number of threads: ";
    cin >> n;
    if (n <= 0) n = 1;

    vector<thread> threads;
    size_t chunkSize = (tasks.size() + n - 1) / n;

    for (int i = 0; i < n; i++) {
        size_t startIdx = i * chunkSize;
        size_t endIdx = min(startIdx + chunkSize, tasks.size());
        if (startIdx >= endIdx) break;

        threads.emplace_back(worker, baseDir, tasks.begin() + startIdx, tasks.begin() + endIdx);

        cout << "🚀 Launched thread " << i + 1 << " handling "
             << endIdx - startIdx << " tracks.\n";

        this_thread::sleep_for(chrono::seconds(6));
    }

    for (auto &t : threads) t.join();
    cout << "🎉 All " << tasks.size() << " tracks processed.\n";
    return 0;
}

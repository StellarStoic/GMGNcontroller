import json
import random
import os
from collections import Counter

def safe_input(prompt):
    user_input = input(prompt).strip()
    if user_input.lower() == "exit":
        print("🚪 Exiting program by user request.")
        exit()
    return user_input

# === Step 1: Check for my_json.json existence ===
if not os.path.isfile('my_json.json'):
    while True:
        choice = safe_input("Do you have a 'my_json.json' file? yes:1 no:0 : ").strip()
        if choice in ["1", "0"]:
            break
        print("Please enter 1 or 0.")

    if choice == "0":
        # Warn if file somehow exists but was not detected
        if os.path.isfile('my_json.json'):
            overwrite = input("I found you already have this file! Creating a new one will overwrite your existing one.\nWhat would you like to do? Create New:1 Don't Create:0 : ").strip()
            if overwrite != "1":
                print("✅ Proceeding without creating a new file.")
            else:
                print("Creating new mockup file...")
        else:
            print("Creating new mockup file...")

        # === Create a mockup JSON ===
        mockup_data = {
            "quotes": [
                {
                    "author": "Marcus Aurelius", "text": "How ridiculous and what a stranger he is who is surprised at anything that happens in life."
                },
                {
                    "author": "Epictetus", "text": "It's not what happens to you, but how you react to it that matters."
                }
            ],
            "fun": {
                "GM": 
                    [
                        "Here's a riddle for you: why did the coffee file a police report?               !̼p̼ǝ̼ᵷ̼ᵷ̼n̼ɯ̼ ʇ̼o̼ᵷ̼ ʇ̼I̼",
                        "Morning brain boost: What is at the end of a rainbow?               ꋖꁝꑀ ꒒ꑀꋖꋖꑀꌅ ꅐ"
                    ],
                "GN": 
                    [
                        "Where do rivers sleep?               I⃝n⃝ r⃝i⃝v⃝e⃝r⃝ b⃝e⃝d⃝s⃝",
                        "What is 8 when 8 is sleeping?               ɿՌԲɿՌɿԵՎ"
                    ]
            },
            "other": {
                "GM": 
                    [
                        "How does a panda make pancakes in the morning?               𝘞̰̾𝘪̰̾𝘵̰̾𝘩̰̾ 𝘢̰̾ 𝘱̰̾𝘢̰̾𝘯̰̾.̰̾.̰̾.̰̾ 𝘥̰̾𝘶̰̾𝘩̰̾!̰̾",
                        "How does the ocean say good morning?               𝙸̴𝚝̴ 𝚠̴𝚊̴𝚟̴𝚎̴𝚜̴!̴"
                    ],
                "GN": 
                    [
                        "Good night! If you hear snoring, it’s just my dreams having a concert!",
                        "Good night! I’m off to count sheep, but I always lose track! ",
                    ]
            },
            "video_audio": {
                "GM": 
                    [
                        "https://youtu.be/rBrd_3VMC3c",
                        "https://youtu.be/7maJOI3QMu0"
                    ],
                "GN": 
                    [
                        "https://youtu.be/S-Xm7s9eGxU",
                        "https://youtu.be/WNcsUNKlAKw""
                    ]
            },
            "images_gifs": {
                "GM": 
                    [
                        "https://media1.tenor.com/m/DeWkpDTdD0cAAAAC/you-awake.gif",
                        "https://media1.tenor.com/m/w1e1N6wc1L8AAAAd/morning-morning-coffee.gif"
                    ],
                "GN": 
                    [
                        "https://media1.tenor.com/m/us2sWg8gAM0AAAAd/cute-sleeping.gif",
                        "https://media1.tenor.com/m/eccDQttkBFkAAAAd/cat-nyash.gif"
                    ]
            },
            "emojis": {
                "GM": 
                    [
                        "🌞",
                        "🌻"
                        "☕️🥐"
                    ],
                "GN": 
                    [
                        "🌙",
                        "🥱"
                        "🌠✨"
                    ]
            }
        }

        with open('my_json.json', 'w', encoding='utf-8') as f:
            json.dump(mockup_data, f, indent=4, ensure_ascii=False)

        print("✅ Mockup 'my_json.json' created.")
        print("⚡ Please edit it and run the program again.")
        exit()

# === Load JSON ===
with open('my_json.json', 'r', encoding='utf-8') as f:
    data = json.load(f)

# === Ask user for GM or GN ===
while True:
    button = safe_input("For which Button are you making sticky strings? (GM or GN): ").strip().upper()
    if button in ["GM", "GN"]:
        break
    print("Please enter either GM or GN.")

# === Ask user if they want to include quotes ===
while True:
    include_quotes = safe_input("Do you want to include quotes? yes: 1 , no: 0 : ").strip()
    if include_quotes in ["1", "0"]:
        include_quotes = include_quotes == "1"
        break
    print("Please enter 1 for yes or 0 for no.")

# === Ask which sections to exclude ===
options = {
    "1": "fun",
    "2": "other",
    "3": "video_audio",
    "4": "images_gifs",
    "5": "emojis"
}

print("\nIs there anything from this list you DO NOT want to include in sticky strings?")
print("Separate numbers with ',' or space:")
for number, name in options.items():
    print(f"{name}: {number}")

exclude_input_raw = safe_input("Exclude: ")

# --- Extract only valid numbers ---
exclude_input = exclude_input_raw.replace(",", " ").split()
exclude_numbers = {num for num in exclude_input if num in options}

excluded_keys = {options[num] for num in exclude_numbers}

if not exclude_numbers and exclude_input_raw.strip() != "":
    print("⚠️  Some or all of your inputs were invalid and ignored.")

print(f"\nYou chose to exclude: {', '.join(excluded_keys) if excluded_keys else 'Nothing'}\n")

# === Collect strings ===
collected_strings = []
duplicate_checker = set()
duplicates = []

# === Statistics ===
category_counter = Counter()

# --- Process general keys ---
for key, value in data.items():
    if key == "quotes" or key in excluded_keys:
        continue
    if isinstance(value, dict):
        entries = value.get(button, [])
        for entry in entries:
            entry_clean = entry.strip()
            if entry_clean:
                if entry_clean not in duplicate_checker:
                    collected_strings.append(entry_clean)
                    duplicate_checker.add(entry_clean)
                    category_counter[key] += 1
                else:
                    duplicates.append((key, entry_clean))

# --- Process quotes if requested ---
if include_quotes:
    for quote in data.get("quotes", []):
        author = quote.get("author", "").strip()
        text = quote.get("text", "").strip()
        if author and text:
            formatted = f"{text} ~ {author}"
            if formatted not in duplicate_checker:
                collected_strings.append(formatted)
                duplicate_checker.add(formatted)
                category_counter["quotes"] += 1
            else:
                duplicates.append(("quotes", formatted))

# === Shuffle ===
random.shuffle(collected_strings)

# === Prepare output ===
output_text = "<<<>>>".join(collected_strings)

# === Save sticky strings ===
output_filename = f"stickyStrings_{button}.txt"
with open(output_filename, 'w', encoding='utf-8') as f:
    f.write(output_text)

# === Save duplicates ===
if duplicates:
    with open('duplicates.txt', 'w', encoding='utf-8') as f:
        for key, value in duplicates:
            f.write(f"[{key}] -> {value}\n")

# === Stats Summary ===
print("\n=== Sticky String Statistics ===")
total = sum(category_counter.values())

for cat, count in category_counter.items():
    percent = (count / total * 100) if total > 0 else 0
    print(f"{cat}: {count} strings = {percent:.2f}%")

print(f"Total sticky strings: {total}")

# === Logging ===
print(f"\n✅ {len(collected_strings)} {button} sticky strings written to {output_filename}.")
print(f"❌ Found {len(duplicates)} duplicates (saved to duplicates.txt)")

if duplicates:
    print("\n--- Duplicate details ---")
    for key, value in duplicates:
        print(f"[{key}] -> {value}")


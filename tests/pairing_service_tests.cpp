#include "oamr/pairing/pairing_service.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>

int main() {
    oamr::pairing::PairingService pairing;

    const auto initial = pairing.current_pair_code();
    assert(initial.size() == 6);
    assert(std::all_of(initial.begin(), initial.end(), [](unsigned char character) {
        return std::isdigit(character) || (character >= 'A' && character <= 'F');
    }));
    assert(pairing.current_pair_code() == initial);

    const auto regenerated = pairing.create_pair_code();
    assert(regenerated.size() == 6);
    assert(pairing.current_pair_code() == regenerated);
}

module;
#include <algorithm>
#include <base/Assert.hh>
#include <base/Macros.hh>
#include <chrono>
#include <functional>
#include <ranges>
#include <SDL3/SDL.h>
#include <thread>
module pr.client;

import base.text;

import pr.utils;
import pr.packets;
import pr.validation;

using namespace pr;
using namespace pr::client;

namespace sc = packets::sc;
namespace cs = packets::cs;

// =============================================================================
//  Error Screen
// =============================================================================
ErrorScreen::ErrorScreen(Client& c) {
    msg = &Create<Label>(
        Position::Center(),
        FontSize::Large,
        TextStyle::Regular,
        TextAlign::Center
    );

    auto& back = Create<Button>(
        c.renderer.make_text("Back", FontSize::Medium),
        Position::HCenter(150)
    );

    back.on_click = [&] { c.enter_screen(*return_screen); };
}

void ErrorScreen::enter(Client& c, std::string t, Screen& return_to) {
    msg->update_text(std::move(t));
    return_screen = &return_to;
    c.enter_screen(*this);
}

// =============================================================================
//  Main Menu Screen
// =============================================================================
MenuScreen::MenuScreen(Client& c) {
    Create<Label>(
        c.renderer.make_text("Prescriptivism", FontSize::Title, TextStyle::Italic),
        Position::HCenter(-50)
    );

    auto& quit = Create<Button>(
        c.renderer.make_text("Quit", FontSize::Medium),
        Position::HCenter(75)
    );

    auto& connect = Create<Button>(
        c.renderer.make_text("Connect", FontSize::Medium),
        Position::HCenter(150)
    );

    auto& address = Create<TextEdit>(
        Position::HCenter(350),
        c.renderer.make_text("Server Address", FontSize::Medium)
    );

    auto& username = Create<TextEdit>(
        Position::HCenter(287),
        c.renderer.make_text("Your Name", FontSize::Medium)
    );

    auto& password = Create<TextEdit>(
        Position::HCenter(225),
        c.renderer.make_text("Password", FontSize::Medium)
    );

    password.set_hide_text(true);

    // FIXME: Testing only. Remove these later.
    address.value(U"localhost");
    username.value(U"testuser");
    password.value(U"password");

    quit.on_click = [&] { c.input_system.quit = true; };
    connect.on_click = [&] {
        c.connexion_screen.enter(
            address.value(),
            username.value(),
            password.value()
        );
    };
}

// =============================================================================
//  Connexion Phase Screens
// =============================================================================
ConnexionScreen::ConnexionScreen(Client& c) : client{c} {
    Create<Label>(
        c.renderer.make_text("Connecting to server...", FontSize::Large),
        Position::HCenter(-100)
    );

    auto& abort = Create<Button>(
        c.renderer.make_text("Abort", FontSize::Medium),
        Position::HCenter(150)
    );

    Create<Throbber>(Position::Center());

    abort.on_click = [&] { st = State::Aborted; };
}

auto ConnexionScreen::connexion_thread_main(
    std::string address,
    std::stop_token st
) -> Result<net::TCPConnexion> {
    // The user may have specified a port; if so, parse it; note
    // that the IPv6 format may contain both colons, so only parse
    // a port in that case if the last one is preceded by a closing
    // square bracket.
    stream s{address};
    u16 port = net::DefaultPort;
    if (s.contains(':') and (s.count(':') == 1 or s.drop_back_until(':').ends_with(']'))) {
        // Just display the port string if it is invalid; the user
        // can figure out why.
        auto port_str = s.take_back_until(':');
        auto parsed_port = Parse<u16>(port_str);
        if (not parsed_port) return Error("Invalid port '{}'", port_str);
        s.drop_back();
        port = parsed_port.value();
    }

    auto sock = net::TCPConnexion::Connect(std::string{s.text()}, port);
    if (st.stop_requested()) return Error("Stop requested");
    return sock;
}

void ConnexionScreen::enter(std::string addr, std::string name, std::string pass) {
    st = State::Entered;
    address = std::move(addr);
    username = std::move(name);
    password = std::move(pass);
    client.enter_screen(*this);
}

void ConnexionScreen::tick(InputSystem& input) {
    Screen::tick(input);
    switch (st) {
        case State::Aborted:
            connexion_thread.stop_and_release();
            client.enter_screen(client.menu_screen);
            break;

        case State::Connecting: {
            if (connexion_thread.running()) return;

            // Connexion thread has exited. Check if we have a connexion.
            auto conn = connexion_thread.value();
            if (not conn) {
                client.show_error(
                    std::format("Connexion failed: {}", conn.error()),
                    client.menu_screen
                );
                return;
            }

            // We do! Tell the server who we are and switch to game screen.
            client.server_connexion = std::move(conn.value());
            client.server_connexion.send(packets::cs::Login(std::move(username), std::move(password)));
            client.enter_screen(client.waiting_screen);
            return;
        }

        case State::Entered:
            if (connexion_thread.running()) return;

            // Restart it.
            st = State::Connecting;
            connexion_thread.start(&ConnexionScreen::connexion_thread_main, this, std::move(address));
            break;
    }
}

void ConnexionScreen::set_address(std::string addr) {
    address = std::move(addr);
}

WaitingScreen::WaitingScreen(Client& c) {
    Create<Throbber>(Position::Center());
    Create<Label>(
        c.renderer.make_text("Waiting for players...", FontSize::Medium),
        Position::Center().voffset(100)
    );
}

WordChoiceScreen::WordChoiceScreen(Client& c) : client{c} {
    cards = &Create<CardGroup>(Position::Center().anchor_to(Anchor::Center));
    cards->autoscale = true;

    // Create dummy cards; we’ll initialise and position them later.
    for (usz i = 0; i < constants::StartingWordSize; i++) cards->add(CardId(i));

    auto& submit = Create<Button>(
        c.renderer.make_text("Submit", FontSize::Medium),
        Position::HCenter(75)
    );

    Create<Label>(
        c.renderer.make_text("Click on a card to select it, then click on a different card to swap them.", FontSize::Medium),
        Position::HCenter(-150)
    );

    submit.on_click = [&] { SendWord(); };
}

void WordChoiceScreen::SendWord() {
    using enum validation::InitialWordValidationResult;
    constants::Word a;
    for (auto [a, c] : vws::zip(a, cards->children)) a = c->id;

    // Validate the word; if it is valid, submit it.
    if (validation::ValidateInitialWord(a, original_word) == Valid) {
        client.server_connexion.send(cs::WordChoice{a});
        client.enter_screen(client.waiting_screen);
        return;
    }

    // If not, tell the user why it wasn’t valid.
    auto msg = [&] -> std::string {
        switch (validation::ValidateInitialWord(a, original_word)) {
            case Valid: break;
            case NotAPermutation:
                return "Error: Not a permutation. This shouldn’t happen; please file a "
                       "bug here: https://github.com/Agma-Schwa/Prescriptivism/issues/new";

            case ClusterTooLong:
                return "Invalid Word: A word must not have more than 2 consecutive "
                       "consonants or vowels.";

            case BadInitialClusterManner:
                return "Invalid Word: A word must not start with M1 or M2 consonant"
                       "followed by another consonant";

            case BadInitialClusterCoordinates:
                return "Invalid Word: If a word starts with a consonant cluster, the"
                       "consonants must not have the same coordinates";
        }
        Unreachable();
    }();

    client.show_error(std::move(msg), *this);
}

void WordChoiceScreen::enter(const constants::Word& word) {
    original_word = word;
    for (auto [w, c] : vws::zip(word, cards->children)) c->id = w;
    client.enter_screen(*this);
}

void WordChoiceScreen::on_refresh(Renderer& r) {
    cards->max_width = r.size().wd;
}

void WordChoiceScreen::tick(InputSystem& input) {
    defer { Screen::tick(input); };

    // Implement card swapping.
    if (input.mouse.left and cards->bounding_box.contains(input.mouse.pos)) {
        auto it = rgs::find_if(
            cards->children,
            [&](auto& c) { return c->bounding_box.contains(input.mouse.pos); }
        );

        // We didn’t click on any card.
        if (it == cards->children.end()) return;

        // If no card is selected, select it.
        u32 idx = u32(it - cards->children.begin());
        if (not selected) {
            it->get()->selected = true;
            selected = idx;
        }

        // If the selected card was clicked, deselect it.
        else if (selected.value() == idx) {
            it->get()->unselect();
            selected = std::nullopt;
        }

        // Otherwise, swap the two and deselect.
        else {
            cards->needs_refresh = true;
            std::iter_swap(cards->children.begin() + selected.value(), it);
            it->get()->unselect(); // Deselect *after* swapping.
            selected = std::nullopt;
        }
    }
}

// =============================================================================
//  Game Screen
// =============================================================================
GameScreen::GameScreen(Client& c) : client(c) {
}

auto GameScreen::PlayerForCardInWord(Card* c) -> Player* {
    Assert(c);
    auto cg = dynamic_cast<CardGroup*>(c->parent);
    if (not cg) return nullptr;
    auto p = player_map.find(cg);
    if (p == player_map.end()) return nullptr;
    return p->second;
}

void GameScreen::ResetOpponentWords() {
    for (auto& p : other_players) {
        p.word->selectable = false;
        p.word->display_state = Card::DisplayState::Default;
    }
}

void GameScreen::TickSelection() {
    if (not selected_element) return;
    auto card = dynamic_cast<Card*>(selected_element);
    Assert(card, "Currently, only cards are selectable");

    // We selected one of our own cards. Make it so we can now select
    // other player’s cards. Unselect the card manually in that case
    // since we don’t want to clear the 'selected' property.
    if (card->parent == our_hand) {
        // We selected the same card again; unselect it this time.
        if (card == our_selected_card) {
            ResetOpponentWords();
            our_selected_card = nullptr;
            card->unselect();
            return;
        }

        // Unselect the previously selected card, set the current selected
        // element of the screen to null, and save it as our selected card;
        if (our_selected_card) our_selected_card->unselect();
        our_selected_card = card;

        // Do not unselect this card as we want to keep it selected.
        selected_element = nullptr;

        // Make other player’s cards selectable if we can play this card on it.
        for (auto& p : other_players) {
            auto cards = p.cards();
            for (auto [i, c] : p.word->children | vws::enumerate) {
                auto v = validation::ValidatePlaySoundCard(our_selected_card->id, cards, i);
                c->selectable = v == validation::PlaySoundCardValidationResult::Valid;
                c->display_state = c->selectable ? Card::DisplayState::Default : Card::DisplayState::Inactive;
            }
        }
        return;
    }

    // Otherwise, we selected another player’s card. We should never get here
    // if we didn’t previously select one of our cards.
    Assert(our_selected_card, "We should have selected one of our cards");
    auto owner = PlayerForCardInWord(card);
    Assert(owner, "Selected card without owner?");

    // Make opponents’ cards non-selectable again.
    // TODO: We’ll need to amend this once we allow selecting multiple cards.
    ResetOpponentWords();

    Log(
        "Targeting opponent {}’s {} with {}",
        owner->name,
        CardDatabase[+card->id].name,
        CardDatabase[+our_selected_card->id].name
    );

    // TODO: Send action to server.
    selected_element->unselect();
    our_selected_card->unselect();
    our_selected_card = nullptr;
}

void GameScreen::enter(sc::StartGame sg) {
    DeleteAllChildren();

    other_players.clear();
    other_words = &Create<Group<>>(Position());
    for (auto [i, p] : sg.player_data | vws::enumerate) {
        if (i == sg.player_id) {
            us = Player("You", sg.player_id);
            us.word = &Create<CardGroup>(Position(), p.word);
            our_hand = &Create<CardGroup>(Position(), sg.hand);
            our_hand->scale = Card::Hand;
            continue;
        }

        auto& op = other_players.emplace_back(std::move(p.name), u8(i));
        op.word = &other_words->Create<CardGroup>(Position(), p.word);
        op.word->scale = Card::OtherPlayer;
    }

    // Build the player map *after* creating all the players, since they
    // might move while in the loop above.
    player_map.clear();
    player_map[us.word] = &us;
    for (auto& p : other_players) player_map[p.word] = &p;

    // The preview must be created at the end so it’s drawn
    // above everything else.
    preview = &Create<Card>(Position::VCenter(-100));
    preview->visible = false;
    preview->hoverable = false;
    preview->scale = Card::Preview;

    // Finally, ‘end’ our turn to reset everything.
    end_turn();
    client.enter_screen(*this);
}

void GameScreen::on_refresh(Renderer&) {
    our_hand->pos = Position::HCenter(50).anchor_to(Anchor::Center);
    us.word->pos = Position::HCenter(400);
    other_words->pos = Position::HCenter(-100);
    other_words->max_gap = 100;
}

void GameScreen::tick(InputSystem& input) {
    if (client.server_connexion.disconnected) {
        client.show_error("Disconnected: Server has gone away", client.menu_screen);
        return;
    }

    Screen::tick(input);
    TickSelection();

    // Preview any card that the user is hovering over.
    auto c = dynamic_cast<Card*>(hovered_element);
    if (c) {
        preview->visible = true;
        preview->id = c->id;
    } else {
        preview->visible = false;
    }
}

void GameScreen::start_turn() {
    our_turn = true;
    for (auto& c : our_hand->children) {
        // Power cards are always usable for now.
        // TODO: Some power cards may not always have valid targets; check for that.
        if (CardDatabase[+c->id].is_power()) {
            c->display_state = Card::DisplayState::Default;
            c->selectable = true;
            continue;
        }

        // For sound cards, check if there are any sounds we can play them on.
        for (auto& p : other_players) {
            auto w = p.cards();
            for (usz i = 0; i < w.size(); i++) {
                auto v = validation::ValidatePlaySoundCard(c->id, w, i);
                if (v == validation::PlaySoundCardValidationResult::Valid) {
                    c->display_state = Card::DisplayState::Default;
                    c->selectable = true;
                    goto next_card;
                }
            }
        }
    next_card:;
    }
}

void GameScreen::end_turn() {
    our_turn = false;
    our_hand->selectable = false;
    our_hand->display_state = Card::DisplayState::Inactive;
    ResetOpponentWords();
}

// =============================================================================
//  Game Screen - Packet Handlers
// =============================================================================
void Client::handle(sc::Disconnect packet) {
    server_connexion.disconnect();
    auto reason = [&] -> std::string_view {
        switch (packet.reason) {
            using Reason = sc::Disconnect::Reason;
            case Reason::Unspecified: return "Disconnected";
            case Reason::ServerFull: return "Disconnected: Server full";
            case Reason::InvalidPacket: return "Disconnected: Client sent invalid packet";
            case Reason::UsernameInUse: return "Disconnected: User name already in use";
            case Reason::WrongPassword: return "Disconnected: Invalid Password";
            case Reason::UnexpectedPacket: return "Disconnected: Unexpected Packet";
            default: return "Disconnected: <<<Invalid>>>";
        }
    }();
    show_error(std::string{reason}, menu_screen);
}

void Client::handle(sc::HeartbeatRequest req) {
    server_connexion.send(cs::HeartbeatResponse{req.seq_no});
}

void Client::handle(sc::WordChoice wc) {
    word_choice_screen.enter(wc.word);
}

void Client::handle(sc::Draw dr) {
    Log("Recieved card: {}", stream(CardDatabase[+dr.card].name).replace("\n", " "));
}

void Client::handle(sc::StartTurn) {
    Assert(current_screen == &game_screen, "StartTurn should only happen after StartGame");
    game_screen.start_turn();
}

void Client::handle(sc::EndTurn) {
    Assert(current_screen == &game_screen, "StartTurn should only happen after StartGame");
    game_screen.end_turn();
}

void Client::handle(sc::StartGame sg) {
    game_screen.enter(std::move(sg));
}

void Client::TickNetworking() {
    if (server_connexion.disconnected) return;
    server_connexion.receive([&](net::ReceiveBuffer& buf) {
        while (not server_connexion.disconnected and not buf.empty()) {
            auto res = packets::HandleClientSidePacket(*this, buf);

            // If there was an error, close the connexion.
            if (not res) {
                server_connexion.disconnect();
                show_error(res.error(), menu_screen);
            }

            // And stop if the packet was incomplete.
            if (not res.value()) break;
        }
    });
}

// =============================================================================
//  API
// =============================================================================
Client::Client(Renderer r) : renderer(std::move(r)) {
    /*
    std::array pi{
        sc::StartGame::PlayerInfo{constants::Word{CardId::C_b, CardId::V_a, CardId::V_e, CardId::C_b, CardId::C_b, CardId::C_b}, "Player"},
        sc::StartGame::PlayerInfo{constants::Word{CardId::C_d, CardId::V_y, CardId::C_d, CardId::C_d, CardId::V_i, CardId::C_d}, "Player"}
    };

    // For testing.
    sc::StartGame sg{pi, {CardId::P_SpellingReform, CardId::P_Chomsky, CardId::V_u}, 0};
    game_screen.enter(sg);
    */
    enter_screen(menu_screen);
}

void Client::Run() {
    Client c{Startup()};
    c.RunGame();
}

void Client::RunAndConnect(std::string address, std::string username, std::string password) {
    Client c{Startup()};
    c.connexion_screen.enter(
        std::move(address),
        std::move(username),
        std::move(password)
    );
    c.RunGame();
}

void Client::enter_screen(Screen& s) {
    current_screen = &s;
    s.refresh(renderer);
    s.on_entered();
}

void Client::Tick() {
    // Handle networking.
    TickNetworking();

    // Start a new frame.
    Renderer::Frame _ = renderer.frame();

    // Refresh screen info.
    current_screen->refresh(renderer);

    // Tick the screen.
    current_screen->tick(input_system);

    // Draw it.
    current_screen->draw(renderer);
}

void Client::RunGame() {
    input_system.game_loop([&] { Tick(); });
}

auto Client::Startup() -> Renderer {
    // Load assets and display a minimal window in the meantime; we
    // can’t access most features of the renderer (e.g. text) while
    // this is happening, but we can clear the screen and draw a
    // throbber.
    //
    // Note: Asset loading doesn’t perform OpenGL calls until we
    // call finalise(), so the reason we can’t do much here is not
    // that another thread is using OpenGl, but rather simply the
    // fact that we don’t have the required assets yet.
    Screen screen;
    Renderer r{1'800, 1'000};
    Thread asset_loader{AssetLoader::Create(r)};
    InputSystem startup{r};
    screen.Create<Throbber>(Position::Center());

    // Flag used to avoid a race condition in case the thread
    // finishes just after the user has pressed 'close' since
    // we set the 'quit' flag of the startup input system to
    // tell it to stop the game loop.
    bool done = false;

    // Display only the throbber until the assets are loaded.
    startup.game_loop([&] {
        Renderer::Frame _ = r.frame();
        screen.draw(r);
        if (not asset_loader.running()) {
            done = true;
            startup.quit = true;
        }
    });

    // If we get here, and 'done' isn’t set, then the user
    // pressed the close button. Also tell the asset loader
    // to stop since we don’t need the assets anymore.
    if (not done) {
        asset_loader.stop_and_release();
        std::exit(0);
    }

    // Finish asset loading.
    asset_loader.value().value().finalise(r);
    InitialiseUI(r);
    return r;
}

void Client::show_error(std::string error, Screen& return_to) {
    error_screen.enter(
        *this,
        std::move(error),
        return_to
    );
}

// Copyright 2014, Georg Sauthoff <mail@georg.so>

/* {{{ GPLv3

    This file is part of imapdl.

    imapdl is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    imapdl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with imapdl.  If not, see <http://www.gnu.org/licenses/>.

}}} */
#include "client.h"

using namespace Memory;

#include "options.h"
#include "enum.h"
#include "exception.h"

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/attributes/named_scope.hpp>
#include <boost/system/error_code.hpp>

#include <sstream>
#include <stdexcept>
#include <string>
#include <functional>
using namespace std;



namespace IMAP {
  namespace Copy {

    State operator++(State &s)
    {
      unsigned i = static_cast<unsigned>(s);
      ++i;
      s = static_cast<State>(i);
      return s;
    }
    State operator+(State s, int b)
    {
      unsigned i = static_cast<unsigned>(s);
      i += b;
      s = static_cast<State>(i);
      return s;
    }
    static const char *const state_map[] = {
      "DISCONNECTED",
      "ESTABLISHED",
      "GOT_INITIAL_CAPABILITIES",
      "LOGGED_IN",
      "GOT_CAPABILITIES",
      "SELECTED_MAILBOX",
      "FETCHING",
      "FETCHED",
      "STORED",
      "EXPUNGED",
      "LOGGING_OUT",
      "LOGGED_OUT",
      "END"
    };
    std::ostream &operator<<(std::ostream &o, State s)
    {
      o << enum_str(state_map, s);
      return o;
    }

    Client::Client(IMAP::Copy::Options &opts,
        std::unique_ptr<Net::Client::Base> &&net_client,
        boost::log::sources::severity_logger< Log::Severity > &lg)
      :
        opts_(opts),
        client_(std::move(net_client)),
        lg_(lg),
        signals_(client_->io_service(), SIGINT, SIGTERM),
        lexer_(buffer_proxy_, tag_buffer_, *this),
        login_timer_(client_->io_service()),
        writer_(tags_, std::bind(&Client::to_cmd, this, std::placeholders::_1)),
        maildir_(opts_.maildir),
        tmp_dir_(maildir_.tmp_dir_fd()),
        fetch_timer_(client_->io_service())
    {
      BOOST_LOG_FUNCTION();
      buffer_proxy_.set(&buffer_);
      do_signal_wait();
      do_resolve();
    }

    void Client::to_cmd(vector<char> &x)
    {
      std::swap(x, cmd_);
    }

    void Client::do_signal_wait()
    {
      signals_.async_wait([this]( const boost::system::error_code &ec, int signal_number)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              if (ec.value() == boost::system::errc::operation_canceled) {
              } else {
                THROW_ERROR(ec);
              }
            } else {
              BOOST_LOG_SEV(lg_, Log::ERROR) << "Got signal: " << signal_number;
              if (signaled_) {
                ostringstream o;
                o << "Got a signal (" << signal_number
                  << ") the second time - immediate exit";
                THROW_MSG(o.str());
              } else {
                ++signaled_;
                do_signal_wait();
                do_quit();
              }
            }
          });
    }

    void Client::do_resolve()
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG(lg_) << "Resolving " << opts_.host << "...";
      client_->async_resolve([this](const boost::system::error_code &ec,
            boost::asio::ip::tcp::resolver::iterator iterator)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              THROW_ERROR(ec);
            } else {
              BOOST_LOG(lg_) << opts_.host << " resolved.";
              do_connect(iterator);
            }
          });
    }

    void Client::do_connect(boost::asio::ip::tcp::resolver::iterator iterator)
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG(lg_) << "Connecting to " << opts_.host << "...";
      client_->async_connect(iterator, [this](const boost::system::error_code &ec)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              THROW_ERROR(ec);
            } else {
              BOOST_LOG(lg_) << opts_.host << " connected.";
              do_handshake();
            }
          });
    }

    void Client::do_handshake()
    {
      BOOST_LOG_FUNCTION();
      if (opts_.use_ssl)
        BOOST_LOG(lg_) << "Cipher list: " << opts_.cipher;
      BOOST_LOG(lg_) << "Shaking hands with " << opts_.host << "...";
      client_->async_handshake([this](const boost::system::error_code &ec)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              THROW_ERROR(ec);
            } else {
              BOOST_LOG(lg_) << "Handshake completed.";
              state_ = State::ESTABLISHED;
              do_read();
              do_pre_login();
            }
          });
    }

    void Client::print_fetch_stats()
    {
      auto fetch_stop = chrono::steady_clock::now();
      auto d = chrono::duration_cast<chrono::milliseconds>
        (fetch_stop - fetch_start_);
      size_t b = client_->bytes_read() - fetch_bytes_start_;
      double r = (double(b)*1024.0)/(double(d.count())*1000.0);
      BOOST_LOG_SEV(lg_, Log::MSG) << "Fetched " << fetched_messages_
        << " messages (" << b << " bytes) in " << double(d.count())/1000.0
        << " s (@ " << r << " KiB/s)";
    }

    void Client::start_fetch_timer()
    {
      fetch_start_ = chrono::steady_clock::now();
      fetch_bytes_start_ = client_->bytes_read();

      resume_fetch_timer();
    }

    void Client::resume_fetch_timer()
    {
      fetch_timer_.expires_from_now(std::chrono::seconds(1));
      fetch_timer_.async_wait([this](const boost::system::error_code &ec)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              if (ec.value() == boost::asio::error::operation_aborted)
                return;
              THROW_ERROR(ec);
            } else {
              print_fetch_stats();
              resume_fetch_timer();
            }
          });
    }
    void Client::stop_fetch_timer()
    {
      print_fetch_stats();
      fetch_timer_.cancel();
    }

    void Client::command()
    {
      BOOST_LOG_FUNCTION();
      switch (state_) {
        case State::FIRST_:
        case State::LAST_:
        case State::DISCONNECTED:
          break;
        case State::ESTABLISHED:
          break;
        case State::GOT_INITIAL_CAPABILITIES:
          do_login();
          break;
        case State::LOGGED_IN:
          do_capabilities();
          break;
        case State::GOT_CAPABILITIES:
          do_select();
          break;
        case State::SELECTED_MAILBOX:
          do_fetch_or_logout();
          break;
        case State::FETCHING:
          // should not visit here in that state
          break;
        case State::FETCHED:
          stop_fetch_timer();
          do_store_or_logout();
          break;
        case State::STORED:
          do_uid_or_simple_expunge();
          break;
        case State::EXPUNGED:
          do_logout();
          break;
        case State::LOGGING_OUT:
          // should not visit here in that state
          break;
        case State::LOGGED_OUT:
          do_quit();
          break;
        case State::END:
          break;
      }
    }

    void Client::do_pre_login()
    {
      login_timer_.expires_from_now(std::chrono::milliseconds(opts_.greeting_wait));
      login_timer_.async_wait([this](
          const boost::system::error_code &ec)
        {
          BOOST_LOG_FUNCTION();
          if (ec) {
            THROW_ERROR(ec);
          } else {
            BOOST_LOG(lg_) << "Point after first possibly occured read";
            do_capabilities();
          }
        });
    }

    void Client::do_capabilities()
    {
      BOOST_LOG_FUNCTION();
      if (!capabilities_.empty()) {
        BOOST_LOG(lg_) << "Switch from state " << state_ << " to " << (state_+1);
        ++state_;
        command();
        return;
      }
      string tag;
      writer_.capability(tag);
      tag_to_state_[tag] = state_ + 1;
      BOOST_LOG(lg_) << "Getting CAPABILITIES ..." << " [" << tag << ']';
      do_write();
    }

    void Client::do_login()
    {
      BOOST_LOG_FUNCTION();
      if (capabilities_.find(IMAP::Server::Response::Capability::LOGINDISABLED)
          != capabilities_.end())
        THROW_MSG("Cannot login because server has LOGINDISABLED");
      BOOST_LOG_SEV(lg_, Log::DEBUG) << "Clearing capabilities";
      capabilities_.clear();
      string tag;
      writer_.login(opts_.username, opts_.password, tag);
      tag_to_state_[tag] = State::LOGGED_IN;

      exists_ = 0;
      recent_ = 0;
      uidvalidity_ = 0;
      uids_.clear();

      BOOST_LOG(lg_) << "Logging in as |" << opts_.username << "| [" << tag << "]";
      BOOST_LOG_SEV(lg_, Log::INSANE) << "Password: |" << opts_.password << "|";
      do_write();
    }

    void Client::do_select()
    {
      BOOST_LOG_FUNCTION();
      string tag;
      writer_.select(opts_.mailbox, tag);
      tag_to_state_[tag] = State::SELECTED_MAILBOX;
      BOOST_LOG(lg_) << "Selecting mailbox: |" << opts_.mailbox << "|" << " [" << tag << ']';
      do_write();
    }

    void Client::do_fetch_or_logout()
    {
      BOOST_LOG_FUNCTION();
      if (exists_) {
        do_fetch();
      } else {
        BOOST_LOG_SEV(lg_, Log::MSG) << "Mailbox " << opts_.mailbox << " is empty.";
        do_logout();
      }
    }

    void Client::do_fetch()
    {
      BOOST_LOG_FUNCTION();
      string tag;

      vector<pair<uint32_t, uint32_t> > set = {
        {1, numeric_limits<uint32_t>::max()}
      };

      using namespace IMAP::Client;
      vector<Fetch_Attribute> atts;
      atts.emplace_back(Fetch::UID);
      atts.emplace_back(Fetch::FLAGS);
      vector<string> fields;
      fields.emplace_back("date");
      fields.emplace_back("from");
      fields.emplace_back("subject");
      // BODY_PEEK - same as BODY but don't set \seen flag ...
      atts.emplace_back(Fetch::BODY_PEEK,
          IMAP::Section_Attribute(IMAP::Section::HEADER_FIELDS, std::move(fields)));
      atts.emplace_back(Fetch::BODY_PEEK);

      writer_.fetch(set, atts, tag);
      tag_to_state_[tag] = State::FETCHED;
      BOOST_LOG(lg_) << "Fetching into " << opts_.maildir
        <<  " ..." << " [" << tag << ']';
      state_ = State::FETCHING;
      start_fetch_timer();
      do_write();
    }

    void Client::do_store_or_logout()
    {
      BOOST_LOG_FUNCTION();
      if (opts_.del) {
        do_store();
      } else {
        do_logout();
      }
    }

    void Client::do_store()
    {
      BOOST_LOG_FUNCTION();
      vector<IMAP::Flag> flags;
      flags.emplace_back(IMAP::Flag::DELETED);

      vector<pair<uint32_t, uint32_t> > set;
      uids_.copy(set);

      string tag;
      writer_.uid_store(set, flags, tag, IMAP::Client::Store_Mode::REPLACE, true);
      tag_to_state_[tag] = State::STORED;
      BOOST_LOG(lg_) << "Storing DELETED flags ..." << " [" << tag << ']';
      do_write();
    }

    bool Client::has_uidplus() const
    {
      BOOST_LOG_FUNCTION();
      auto i = capabilities_.find(IMAP::Server::Response::Capability::UIDPLUS);
      BOOST_LOG(lg_) << "Has UIDPLUS capability: " << (i != capabilities_.end());
      return i != capabilities_.end();
    }
    
    void Client::do_uid_or_simple_expunge()
    {
      BOOST_LOG_FUNCTION();
      if (has_uidplus())
        do_uid_expunge();
      else
        do_expunge();
    }

    void Client::do_uid_expunge()
    {
      BOOST_LOG_FUNCTION();
      string tag;
      vector<pair<uint32_t, uint32_t> > set;
      uids_.copy(set);
      writer_.uid_expunge(set, tag);
      tag_to_state_[tag] = State::EXPUNGED;
      BOOST_LOG(lg_) << "Expunging messages ..." << " [" << tag << ']';
      do_write();
    }

    void Client::do_expunge()
    {
      BOOST_LOG_FUNCTION();
      string tag;
      writer_.expunge(tag);
      tag_to_state_[tag] = State::EXPUNGED;
      BOOST_LOG(lg_) << "Expunging messages (without UIDPLUS) ..." << " [" << tag << ']';
      do_write();
    }

    void Client::do_logout()
    {
      BOOST_LOG_FUNCTION();
      string tag;
      writer_.logout(tag);
      tag_to_state_[tag] = State::LOGGED_OUT;
      BOOST_LOG(lg_) << "Logging out ..." << " [" << tag << ']';
      state_ = State::LOGGING_OUT;
      do_write();
    }

    void Client::do_read()
    {
      BOOST_LOG_FUNCTION();
      client_->async_read_some([this](
            const boost::system::error_code &ec,
            size_t size)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              if (          state_ == State::LOGGED_OUT
                  && (   (    ec.category() == boost::asio::error::get_ssl_category()
                           && ec.value()    == ERR_PACK(ERR_LIB_SSL, 0, SSL_R_SHORT_READ)
                         )
                      || (    ec.category() == boost::asio::error::get_misc_category()
                           && ec.value()    == boost::asio::error::eof
                         )
                     )
                )
                  {
                //BOOST_LOG_SEV(lg_, Log::DEBUG) << "do_read() -> do_shutdown()";
                //do_shutdown();
              } else {
                BOOST_LOG_SEV(lg_, Log::DEBUG) << "do_read() fail: " << ec.message();
                THROW_ERROR(ec);
              }
            } else {
              lexer_.read(client_->input().data(), client_-> input().data() + size);
              if (state_ != State::LOGGED_OUT) // && client_->is_open())
                do_read();
            }
          });
    }

    void Client::do_write()
    {
      client_->push_write(cmd_);
    }

    void Client::do_quit()
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG_SEV(lg_, Log::DEBUG) << "do_quit()";
      client_->cancel();
      do_shutdown();
    }

    void Client::do_shutdown()
    {
      BOOST_LOG_FUNCTION();
      client_->async_shutdown([this](
            const boost::system::error_code &ec)
          {
            BOOST_LOG_FUNCTION();
            if (ec) {
              if (   ec.category() == boost::asio::error::get_ssl_category()
                  && ec.value()    == ERR_PACK(ERR_LIB_SSL, 0, SSL_R_SHORT_READ)) {
              } else if (   ec.category() == boost::asio::error::get_ssl_category()
                         && ERR_GET_REASON(ec.value())
                              == SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC) {
              } else {
                if (ec.category() == boost::asio::error::get_ssl_category()) {
                  BOOST_LOG_SEV(lg_, Log::ERROR)
                    << "ssl_category: lib " << ERR_GET_LIB(ec.value())
                    << " func " << ERR_GET_FUNC(ec.value())
                    << " reason " << ERR_GET_REASON(ec.value());
                }
                BOOST_LOG_SEV(lg_, Log::DEBUG) << "do_shutdown() fail: " << ec.message();
                THROW_ERROR(ec);
              }
            } else {
            }
            client_->close();
            signals_.cancel();
          });
    }


    void Client::imap_status_code_capability_begin()
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG_SEV(lg_, Log::DEBUG) << "Clearing capabilities";
      capabilities_.clear();
    }
    void Client::imap_capability_begin()
    {
    }
    void Client::imap_capability(IMAP::Server::Response::Capability capability)
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG(lg_) << "Got capability: " << capability;
      capabilities_.insert(capability);
    }
    void Client::imap_tagged_status_end(IMAP::Server::Response::Status c)
    {
      BOOST_LOG_FUNCTION();
      string tag(tag_buffer_.begin(), tag_buffer_.end());
      BOOST_LOG(lg_) << "Got status " << c << " for tag " << tag;
      if (c != IMAP::Server::Response::Status::OK) {
        stringstream o;
        o << "Command failed: " << c << " - " << string(buffer_.begin(), buffer_.end());
        THROW_MSG(o.str());
      }
      auto i = tag_to_state_.find(tag);
      if (i == tag_to_state_.end()) {
        stringstream o;
        o << "Got unknown tag: " << tag;
        THROW_MSG(o.str());
      }
      BOOST_LOG(lg_) << "Switch from state " << state_ << " to " << i->second
        << " [" << tag << "]";
      state_ = i->second;
      tag_to_state_.erase(i);
      command();
    }
    void Client::imap_data_exists(uint32_t number)
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG(lg_) << "Mailbox " << opts_.mailbox << " contains " << number
        << " messages";
      exists_ = number;
    }
    void Client::imap_data_recent(uint32_t number)
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG(lg_) << "Mailbox " << opts_.mailbox << " has " << number
        << " RECENT messages";
      recent_ = number;
    }
    void Client::imap_status_code_uidvalidity(uint32_t n)
    {
      BOOST_LOG_FUNCTION();
      BOOST_LOG(lg_) << "UIDVALIDITY: " << n;
      uidvalidity_ = n;
    }

    void Client::imap_data_fetch_begin(uint32_t number)
    {
      BOOST_LOG_FUNCTION();
      flags_.clear();
      if (state_ == State::FETCHING) {
        BOOST_LOG(lg_) << "Fetching message: " << number;
      }
    }
    void Client::imap_data_fetch_end()
    {
    }
    void Client::imap_section_empty()
    {
      full_body_ = true;
    }
    void Client::imap_body_section_inner()
    {
      if (state_ == State::FETCHING) {
        if (full_body_) {
          string filename;
          maildir_.create_tmp_name(filename);
          Buffer::File f(tmp_dir_, filename);
          file_buffer_ = std::move(f);
          buffer_proxy_.set(&file_buffer_);
        }
      }
    }
    void Client::imap_body_section_end()
    {
      BOOST_LOG_FUNCTION();
      if (state_ == State::FETCHING) {
        if (full_body_) {
          buffer_proxy_.set(&buffer_);
          file_buffer_.close();
          if (flags_.empty()) {
            maildir_.move_to_new();
          } else  {
            BOOST_LOG_SEV(lg_, Log::DEBUG) << "Using maildir flags: " << flags_;
            maildir_.move_to_cur(flags_);
          }
          full_body_ = false;
          ++fetched_messages_;
        }
      }
    }
    void Client::imap_flag(Flag flag)
    {
      switch (flag) {
        case IMAP::Flag::ANSWERED:
          flags_.push_back('R');
          break;
        case IMAP::Flag::SEEN:
          flags_.push_back('S');
          break;
        case IMAP::Flag::FLAGGED:
          flags_.push_back('F');
          break;
        case IMAP::Flag::DRAFT:
          flags_.push_back('D');
          break;
        case IMAP::Flag::RECENT:
        case IMAP::Flag::DELETED:
        case IMAP::Flag::FIRST_:
        case IMAP::Flag::LAST_:
          break;
      }
    }
    void Client::imap_uid(uint32_t number)
    {
      BOOST_LOG_FUNCTION();
      if (state_ == State::FETCHING) {
        BOOST_LOG_SEV(lg_, Log::DEBUG) << "UID: " << number;
        uids_.push(number);
      }
    }

  }
}

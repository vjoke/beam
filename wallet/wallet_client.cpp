// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wallet_client.h"
#include "utility/log_rotation.h"
#include "../core/block_rw.h"


using namespace std;

namespace
{
    using namespace beam;
    using namespace beam::wallet;

template<typename Observer, typename Notifier>
struct ScopedSubscriber
{
    ScopedSubscriber(Observer* observer, const std::shared_ptr<Notifier>& notifier)
        : m_observer(observer)
        , m_notifier(notifier)
    {
        m_notifier->subscribe(m_observer);
    }

    ~ScopedSubscriber()
    {
        m_notifier->unsubscribe(m_observer);
    }
private:
    Observer * m_observer;
    std::shared_ptr<Notifier> m_notifier;
};

using WalletSubscriber = ScopedSubscriber<wallet::IWalletObserver, wallet::IWallet>;

struct WalletModelBridge : public Bridge<IWalletModelAsync>
{
    BRIDGE_INIT(WalletModelBridge);

    void sendMoney(const wallet::WalletID& receiverID, const std::string& comment, Amount&& amount, Amount&& fee) override
    {
        typedef void(IWalletModelAsync::*SendMoneyType)(const wallet::WalletID&, const std::string&, Amount&&, Amount&&);
        call_async((SendMoneyType)&IWalletModelAsync::sendMoney, receiverID, comment, move(amount), move(fee));
    }

    void sendMoney(const wallet::WalletID& senderID, const wallet::WalletID& receiverID, const std::string& comment, Amount&& amount, Amount&& fee) override
    {
        typedef void(IWalletModelAsync::*SendMoneyType)(const wallet::WalletID &, const wallet::WalletID &, const std::string &, Amount &&, Amount &&);
        call_async((SendMoneyType)&IWalletModelAsync::sendMoney, senderID, receiverID, comment, move(amount), move(fee));
    }

    void syncWithNode() override
    {
        call_async(&IWalletModelAsync::syncWithNode);
    }

    void calcChange(Amount&& amount) override
    {
        call_async(&IWalletModelAsync::calcChange, move(amount));
    }

    void getWalletStatus() override
    {
        call_async(&IWalletModelAsync::getWalletStatus);
    }

    void getUtxosStatus() override
    {
        call_async(&IWalletModelAsync::getUtxosStatus);
    }

    void getAddresses(bool own) override
    {
        call_async(&IWalletModelAsync::getAddresses, own);
    }

    void cancelTx(const wallet::TxID& id) override
    {
        call_async(&IWalletModelAsync::cancelTx, id);
    }

    void deleteTx(const wallet::TxID& id) override
    {
        call_async(&IWalletModelAsync::deleteTx, id);
    }

    void getCoinsByTx(const wallet::TxID& id) override
    {
        call_async(&IWalletModelAsync::getCoinsByTx, id);
    }

    void saveAddress(const wallet::WalletAddress& address, bool bOwn) override
    {
        call_async(&IWalletModelAsync::saveAddress, address, bOwn);
    }

    void changeCurrentWalletIDs(const wallet::WalletID& senderID, const wallet::WalletID& receiverID) override
    {
        call_async(&IWalletModelAsync::changeCurrentWalletIDs, senderID, receiverID);
    }

    void generateNewAddress() override
    {
        call_async(&IWalletModelAsync::generateNewAddress);
    }

    void deleteAddress(const wallet::WalletID& id) override
    {
        call_async(&IWalletModelAsync::deleteAddress, id);
    }

    void updateAddress(const wallet::WalletID& id, const std::string& name, WalletAddress::ExpirationStatus status) override
    {
        call_async(&IWalletModelAsync::updateAddress, id, name, status);
    }

    void setNodeAddress(const std::string& addr) override
    {
        call_async(&IWalletModelAsync::setNodeAddress, addr);
    }

    void changeWalletPassword(const SecString& pass) override
    {
        // TODO: should be investigated, don't know how to "move" SecString into lambda
        std::string passStr(pass.data(), pass.size());

        call_async(&IWalletModelAsync::changeWalletPassword, passStr);
    }

    void getNetworkStatus() override
    {
        call_async(&IWalletModelAsync::getNetworkStatus);
    }

    void refresh() override
    {
        call_async(&IWalletModelAsync::refresh);
    }

    void exportPaymentProof(const wallet::TxID& id) override
    {
        call_async(&IWalletModelAsync::exportPaymentProof, id);
    }

    void checkAddress(const std::string& addr) override
    {
        call_async(&IWalletModelAsync::checkAddress, addr);
    }
};
}

namespace beam::wallet
{
    WalletClient::WalletClient(IWalletDB::Ptr walletDB, const std::string& nodeAddr, io::Reactor::Ptr reactor)
        : m_walletDB(walletDB)
        , m_reactor{ reactor ? reactor : io::Reactor::create() }
        , m_async{ make_shared<WalletModelBridge>(*(static_cast<IWalletModelAsync*>(this)), *m_reactor) }
        , m_isConnected(false)
        , m_nodeAddrStr(nodeAddr)
    {
    }

    WalletClient::~WalletClient()
    {
        // reactor should be already stopped here, but just in case
        // this call is unsafe and may result in crash if reactor is not stopped
        assert(!m_thread && !m_reactor);
        stopReactor();
    }

    void WalletClient::stopReactor()
    {
        try
        {
            if (m_reactor)
            {
                if (m_thread)
                {
                    m_reactor->stop();
                    m_thread->join();
                    m_thread.reset();
                }
                m_reactor.reset();
            }
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::start()
    {
        m_thread = std::make_shared<std::thread>([this]()
            {
                try
                {
					io::Reactor::Scope scope(*m_reactor);
					io::Reactor::GracefulIntHandler gih(*m_reactor);

					std::unique_ptr<WalletSubscriber> wallet_subscriber;

                    onStatus(getStatus());
                    onTxStatus(ChangeAction::Reset, m_walletDB->getTxHistory());

                    static const unsigned LOG_ROTATION_PERIOD_SEC = 3 * 3600; // 3 hours
                    static const unsigned LOG_CLEANUP_PERIOD_SEC = 120 * 3600; // 5 days
                    LogRotation logRotation(*m_reactor, LOG_ROTATION_PERIOD_SEC, LOG_CLEANUP_PERIOD_SEC);

                    auto wallet = make_shared<Wallet>(m_walletDB);
                    m_wallet = wallet;

                    class NodeNetwork final: public proto::FlyClient::NetworkStd
                    {
                    public:
                        NodeNetwork(proto::FlyClient& fc, WalletClient& client, const std::string& nodeAddress)
                            : proto::FlyClient::NetworkStd(fc)
                            , m_nodeAddrStr(nodeAddress)
                            , m_walletClient(client)
                        {
                        }

                        void tryToConnect()
                        {
                            // if user changed address to correct (using of setNodeAddress)
                            if (m_Cfg.m_vNodes.size() > 0)
                                return;

                            if (!m_timer)
                            {
                                m_timer = io::Timer::create(io::Reactor::get_Current());
                            }

                            if (m_attemptToConnect < MAX_ATTEMPT_TO_CONNECT)
                            {
                                ++m_attemptToConnect;
                            }
                            else if (m_attemptToConnect == MAX_ATTEMPT_TO_CONNECT)
                            {
                                proto::NodeConnection::DisconnectReason reason;
                                reason.m_Type = proto::NodeConnection::DisconnectReason::Io;
                                reason.m_IoError = io::EC_HOST_RESOLVED_ERROR;
                                m_walletClient.nodeConnectionFailed(reason);
                            }

                            m_timer->start(RECONNECTION_TIMEOUT, false, [this]() {
                                io::Address nodeAddr;
                                if (nodeAddr.resolve(m_nodeAddrStr.c_str()))
                                {
                                    m_Cfg.m_vNodes.push_back(nodeAddr);
                                    Connect();
                                }
                                else
                                {
                                    tryToConnect();
                                }
                            });
                        }

                    private:
                        void OnNodeConnected(size_t, bool bConnected) override
                        {
                            m_walletClient.nodeConnectedStatusChanged(bConnected);
                        }

                        void OnConnectionFailed(size_t, const proto::NodeConnection::DisconnectReason& reason) override
                        {
                            m_walletClient.nodeConnectionFailed(reason);
                        }

                    public:
                        std::string m_nodeAddrStr;
                        WalletClient& m_walletClient;

                        io::Timer::Ptr m_timer;
                        uint8_t m_attemptToConnect = 0;

                        const uint8_t MAX_ATTEMPT_TO_CONNECT = 5;
                        const uint16_t RECONNECTION_TIMEOUT = 1000;
                    };

                    auto nodeNetwork = make_shared<NodeNetwork>(*wallet, *this, m_nodeAddrStr);
                    m_nodeNetwork = nodeNetwork;

                    auto walletNetwork = make_shared<WalletNetworkViaBbs>(*wallet, nodeNetwork, m_walletDB);
                    m_walletNetwork = walletNetwork;
                    wallet->SetNodeEndpoint(nodeNetwork);
                    wallet->AddMessageEndpoint(walletNetwork);

                    wallet_subscriber = make_unique<WalletSubscriber>(static_cast<IWalletObserver*>(this), wallet);

                    nodeNetwork->tryToConnect();
                    m_reactor->run_ex([&wallet, &nodeNetwork](){
                        wallet->CleanupNetwork();
                        nodeNetwork->Disconnect();
                    });

                    assert(walletNetwork.use_count() == 1);
                    walletNetwork.reset();

                    assert(nodeNetwork.use_count() == 1);
                    nodeNetwork.reset();
                }
                catch (const runtime_error& ex)
                {
                    LOG_ERROR() << ex.what();
                    FailedToStartWallet();
                }
                catch (...) {
                    LOG_UNHANDLED_EXCEPTION();
                }
            });
    }

    IWalletModelAsync::Ptr WalletClient::getAsync()
    {
        return m_async;
    }

    std::string WalletClient::getNodeAddress() const
    {
        return m_nodeAddrStr;
    }

    std::string WalletClient::exportOwnerKey(const beam::SecString& pass) const
    {
        Key::IKdf::Ptr pKey = m_walletDB->get_MasterKdf();
        const ECC::HKdf& kdf = static_cast<ECC::HKdf&>(*pKey);

        KeyString ks;
        ks.SetPassword(Blob(pass.data(), static_cast<uint32_t>(pass.size())));
        ks.m_sMeta = std::to_string(0);

        ECC::HKdfPub pkdf;
        pkdf.GenerateFrom(kdf);

        ks.Export(pkdf);

        return ks.m_sRes;
    }

    bool WalletClient::isRunning() const
    {
        return m_thread && m_thread->joinable();
    }

    bool WalletClient::isFork1() const
    {
        return m_walletDB->getCurrentHeight() >= Rules::get().pForks[1].m_Height;
    }

    void WalletClient::onCoinsChanged()
    {
        onAllUtxoChanged(getUtxos());
        // TODO may be it needs to delete
        onStatus(getStatus());
    }

    void WalletClient::onTransactionChanged(ChangeAction action, std::vector<TxDescription>&& items)
    {
        onTxStatus(action, move(items));
        onStatus(getStatus());
    }

    void WalletClient::onSystemStateChanged()
    {
        onStatus(getStatus());
    }

    void WalletClient::onAddressChanged(ChangeAction action, const std::vector<WalletAddress>& items)
    {
        // TODO: need to change this behavior
        onAddresses(true, m_walletDB->getAddresses(true));
        onAddresses(false, m_walletDB->getAddresses(false));
    }

    void WalletClient::onSyncProgress(int done, int total)
    {
        onSyncProgressUpdated(done, total);
    }

    void WalletClient::sendMoney(const WalletID& receiver, const std::string& comment, Amount&& amount, Amount&& fee)
    {
        try
        {
            WalletAddress senderAddress = storage::createAddress(*m_walletDB);
            saveAddress(senderAddress, true); // should update the wallet_network

            ByteBuffer message(comment.begin(), comment.end());

            assert(!m_wallet.expired());
            auto s = m_wallet.lock();
            if (s)
            {
                s->transfer_money(senderAddress.m_walletID, receiver, move(amount), move(fee), true, kDefaultTxLifetime, kDefaultTxResponseTime, move(message), true);
            }

            onSendMoneyVerified();
        }
        catch (const CannotGenerateSecretException&)
        {
            onNewAddressFailed();
            return;
        }
        catch (const AddressExpiredException&)
        {
            onCantSendToExpired();
            return;
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::sendMoney(const WalletID& sender, const WalletID& receiver, const std::string& comment, Amount&& amount, Amount&& fee)
    {
        try
        {
            ByteBuffer message(comment.begin(), comment.end());

            assert(!m_wallet.expired());
            auto s = m_wallet.lock();
            if (s)
            {
                s->transfer_money(sender, receiver, move(amount), move(fee), true, kDefaultTxLifetime, kDefaultTxResponseTime, move(message), true);
            }

            onSendMoneyVerified();
        }
        catch (const CannotGenerateSecretException&)
        {
            onNewAddressFailed();
            return;
        }
        catch (const AddressExpiredException&)
        {
            onCantSendToExpired();
            return;
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::syncWithNode()
    {
        assert(!m_nodeNetwork.expired());
        auto s = m_nodeNetwork.lock();
        if (s)
            s->Connect();
    }

    void WalletClient::calcChange(Amount&& amount)
    {
        auto coins = m_walletDB->selectCoins(amount);
        Amount sum = 0;
        for (auto& c : coins)
        {
            sum += c.m_ID.m_Value;
        }
        if (sum < amount)
        {
            onChangeCalculated(0);
        }
        else
        {
            onChangeCalculated(sum - amount);
        }
    }

    void WalletClient::getWalletStatus()
    {
        onStatus(getStatus());
        onTxStatus(ChangeAction::Reset, m_walletDB->getTxHistory());
        onAddresses(false, m_walletDB->getAddresses(false));
        onAddresses(true, m_walletDB->getAddresses(true));
    }

    void WalletClient::getUtxosStatus()
    {
        onStatus(getStatus());
        onAllUtxoChanged(getUtxos());
    }

    void WalletClient::getAddresses(bool own)
    {
        onAddresses(own, m_walletDB->getAddresses(own));
    }

    void WalletClient::cancelTx(const TxID& id)
    {
        auto w = m_wallet.lock();
        if (w)
        {
            static_pointer_cast<IWallet>(w)->cancel_tx(id);
        }
    }

    void WalletClient::deleteTx(const TxID& id)
    {
        auto w = m_wallet.lock();
        if (w)
        {
            static_pointer_cast<IWallet>(w)->delete_tx(id);
        }
    }

    void WalletClient::getCoinsByTx(const TxID& id)
    {
        onCoinsByTx(m_walletDB->getCoinsByTx(id));
    }

    void WalletClient::saveAddress(const WalletAddress& address, bool bOwn)
    {
        m_walletDB->saveAddress(address);
    }

    void WalletClient::changeCurrentWalletIDs(const WalletID& senderID, const WalletID& receiverID)
    {
        onChangeCurrentWalletIDs(senderID, receiverID);
    }

    void WalletClient::generateNewAddress()
    {
        try
        {
            WalletAddress address = storage::createAddress(*m_walletDB);

            onGeneratedNewAddress(address);
        }
        catch (const CannotGenerateSecretException&)
        {
            onNewAddressFailed();
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::deleteAddress(const WalletID& id)
    {
        try
        {
            auto pVal = m_walletDB->getAddress(id);
            if (pVal)
            {
                m_walletDB->deleteAddress(id);
            }
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::updateAddress(const WalletID& id, const std::string& name, WalletAddress::ExpirationStatus status)
    {
        try
        {
            auto addr = m_walletDB->getAddress(id);

            if (addr)
            {
                if (addr->m_OwnID)
                {
                    addr->setExpiration(status);
                }
                addr->setLabel(name);
                m_walletDB->saveAddress(*addr);
            }
            else
            {
                LOG_ERROR() << "Address " << to_string(id) << " is absent.";
            }
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::setNodeAddress(const std::string& addr)
    {
        io::Address nodeAddr;

        if (nodeAddr.resolve(addr.c_str()))
        {
            m_nodeAddrStr = addr;

            assert(!m_nodeNetwork.expired());
            auto s = m_nodeNetwork.lock();
            if (s)
            {
                s->Disconnect();

                static_cast<proto::FlyClient::NetworkStd&>(*s).m_Cfg.m_vNodes.clear();
                static_cast<proto::FlyClient::NetworkStd&>(*s).m_Cfg.m_vNodes.push_back(nodeAddr);

                s->Connect();
            }
        }
        else
        {
            LOG_ERROR() << "Unable to resolve node address: " << addr;
            onWalletError(ErrorType::HostResolvedError);
        }
    }

    void WalletClient::changeWalletPassword(const SecString& pass)
    {
        m_walletDB->changePassword(pass);
    }

    void WalletClient::getNetworkStatus()
    {
        if (m_walletError.is_initialized() && !m_isConnected)
        {
            onWalletError(*m_walletError);
            return;
        }

        onNodeConnectionChanged(m_isConnected);
    }

    void WalletClient::refresh()
    {
        try
        {
            assert(!m_wallet.expired());
            auto s = m_wallet.lock();
            if (s)
            {
                s->Refresh();
            }
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }
    }

    void WalletClient::exportPaymentProof(const TxID& id)
    {
        onPaymentProofExported(id, storage::ExportPaymentProof(*m_walletDB, id));
    }

    void WalletClient::checkAddress(const std::string& addr)
    {
        io::Address nodeAddr;

        onAddressChecked(addr, nodeAddr.resolve(addr.c_str()));
    }

    WalletStatus WalletClient::getStatus() const
    {
        WalletStatus status;
        storage::Totals totals(*m_walletDB);

        status.available = totals.Avail;
        status.receiving = totals.Incoming;
        status.sending = totals.Outgoing;
        status.maturing = totals.Maturing;

        status.update.lastTime = m_walletDB->getLastUpdateTime();

        ZeroObject(status.stateID);
        m_walletDB->getSystemStateID(status.stateID);

        return status;
    }

    vector<Coin> WalletClient::getUtxos() const
    {
        vector<Coin> utxos;
        m_walletDB->visitCoins([&utxos](const Coin& c)->bool
            {
                utxos.push_back(c);
                return true;
            });
        return utxos;
    }

    void WalletClient::nodeConnectionFailed(const proto::NodeConnection::DisconnectReason& reason)
    {
        m_isConnected = false;

        // reason -> ErrorType
        if (proto::NodeConnection::DisconnectReason::ProcessingExc == reason.m_Type)
        {
            m_walletError = getWalletError(reason.m_ExceptionDetails.m_ExceptionType);
            onWalletError(*m_walletError);
            return;
        }

        if (proto::NodeConnection::DisconnectReason::Io == reason.m_Type)
        {
            m_walletError = getWalletError(reason.m_IoError);
            onWalletError(*m_walletError);
            return;
        }

        LOG_ERROR() << "Unprocessed error: " << reason;
    }

    void WalletClient::nodeConnectedStatusChanged(bool isNodeConnected)
    {
        m_isConnected = isNodeConnected;
        onNodeConnectionChanged(isNodeConnected);
    }
}
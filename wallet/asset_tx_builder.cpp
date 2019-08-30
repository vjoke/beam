// Copyright 2019 The Beam Team
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

#include "common.h"
#include "asset_tx_builder.h"
#include "utility/logger.h"

#include <boost/uuid/uuid_generators.hpp>
#include <numeric>

using namespace ECC;
using namespace std;

namespace beam::wallet
{
AssetTxBuilder::AssetTxBuilder(BaseTransaction &tx, SubTxID subTxID, Amount fee, const AmountList &assetAmountList, AssetID assetID)
    : BaseTxBuilder(tx, subTxID, {}, fee), m_AssetAmountList{assetAmountList}, m_AssetID{assetID}, m_AssetChange{0}
{
    if (m_AssetAmountList.empty())
    {
        m_Tx.GetParameter(TxParameterID::AssetAmountList, m_AssetAmountList, m_SubTxID);
    }
}


Amount AssetTxBuilder::GetAssetAmount() const
{
    return std::accumulate(m_AssetAmountList.begin(), m_AssetAmountList.end(), 0ULL);
}

const AmountList& AssetTxBuilder::GetAssetAmountList() const
{
    return m_AssetAmountList;
}

void AssetTxBuilder::SelectInputs()
{
    LOG_INFO() << "AssetTxBuilder::SelectInputs called";
    // Firstly, select input for main coin
    BaseTxBuilder::SelectInputs();
    auto assetID = m_Tx.GetMandatoryParameter<AssetID>(TxParameterID::AssetID, m_SubTxID);
    if (assetID == Zero)
    {
        LOG_ERROR() << m_Tx.GetTxID() << " invalid asset ";
        throw TransactionFailedException(true, TxFailureReason::InvalidTransaction);
    }

    auto assetCommand = m_Tx.GetMandatoryParameter<AssetCommand>(TxParameterID::AssetCommand, m_SubTxID);
    
    switch (assetCommand) {
        case AssetCommand::Issue:
        {
            LOG_INFO() << m_Tx.GetTxID() << " issues asset " << GetAssetAmount() << " with asset id: " << assetID;
        }
        break;
        case AssetCommand::Transfer:
        {

        }
        break;
        case AssetCommand::Burn:
        {

        }
        break;
        default:
        {
            LOG_ERROR() << m_Tx.GetTxID() << " invalid asset command: " << uint8_t(assetCommand);
            throw TransactionFailedException(true, TxFailureReason::InvalidTransaction);
        }
        break;

    }
}

void AssetTxBuilder::AddChange()
{
    BaseTxBuilder::AddChange();
    if (m_AssetChange == 0)
    {
        return;
    }

    GenerateNewCoin(m_AssetChange, true);
}

void AssetTxBuilder::GenerateNewCoin(Amount amount, bool bChange)
{
    LOG_INFO() << "AssetTxBuilder::GenerateNewCoin called";
    Coin newUtxo{ amount, Key::Type::Regular, m_AssetID };
    newUtxo.m_createTxId = m_Tx.GetTxID();
    if (bChange)
    {
        newUtxo.m_ID.m_Type = Key::Type::Change;
    }
    m_Tx.GetWalletDB()->storeCoin(newUtxo);
    m_AssetOutputCoins.push_back(newUtxo.m_ID);
    // FIXME: 
    m_Tx.SetParameter(TxParameterID::AssetOutputCoins, m_AssetOutputCoins, false, m_SubTxID);
}

void AssetTxBuilder:: GenerateNewCoinList(bool bChange)
{
    BaseTxBuilder::GenerateNewCoinList(bChange);
    for (const auto &amount : GetAssetAmountList())
    {
        GenerateNewCoin(amount, bChange);
    }
}

void AssetTxBuilder::GenerateOffset()
{
    // FIXME:
    BaseTxBuilder::GenerateOffset();
}

bool AssetTxBuilder::CreateOutputs()
{
    BaseTxBuilder::CreateOutputs();

    if (GetAssetOutputs() || GetAssetOutputCoins().empty())
    {
        return false;
    }

    auto thisHolder = shared_from_this();
    auto txHolder = m_Tx.shared_from_this(); // increment use counter of tx object. We use it to avoid tx object desctruction during Update call.
    m_Tx.GetAsyncAcontext().OnAsyncStarted();
    m_Tx.GetKeyKeeper()->GenerateOutputs(m_MinHeight, m_AssetOutputCoins,
        [thisHolder, this, txHolder](auto &&result) {
            m_AssetOutputs = move(result);
            FinalizeAssetOutputs();
            m_Tx.Update(); // may complete tranasction
            m_Tx.GetAsyncAcontext().OnAsyncFinished();
        },
        [thisHolder, this, txHolder](const exception &) {
            //m_Tx.Update();
            m_Tx.GetAsyncAcontext().OnAsyncFinished();
        },
        m_AssetID);
    return true; // true if async
}

bool AssetTxBuilder::FinalizeAssetOutputs()
{
    m_Tx.SetParameter(TxParameterID::AssetOutputs, m_AssetOutputs, false, m_SubTxID);

    // TODO: check transaction size here

    return true;
}

bool AssetTxBuilder::CreateInputs()
{
    bool ret = BaseTxBuilder::CreateInputs();

    if (GetAssetInputs() || GetAssetInputCoins().empty())
    {
        return false;
    }

    auto commitments = m_Tx.GetKeyKeeper()->GeneratePublicKeysSync(m_AssetInputCoins, true, &m_AssetID);
    m_Inputs.reserve(commitments.size());
    for (const auto &commitment : commitments)
    {
        auto &input = m_AssetInputs.emplace_back(make_unique<Input>());
        input->m_Commitment = commitment;
    }
    FinalizeAssetInputs();
    return false; // true if async operation has run
}

void AssetTxBuilder::FinalizeAssetInputs()
{
    m_Tx.SetParameter(TxParameterID::AssetInputs, m_AssetInputs, false, m_SubTxID);
}

bool AssetTxBuilder::FinalizeOutputs()
{
    return BaseTxBuilder::FinalizeOutputs();
}

bool AssetTxBuilder::GetPeerInputsAndOutputs()
{
    LOG_INFO() << "GetPeerInputsAndOutputs in asset builder";
    return BaseTxBuilder::GetPeerInputsAndOutputs();
}

bool AssetTxBuilder::GetAssetInputs()
{
    return m_Tx.GetParameter(TxParameterID::AssetInputs, m_AssetInputs, m_SubTxID);
}

bool AssetTxBuilder::GetAssetOutputs()
{
    return m_Tx.GetParameter(TxParameterID::AssetOutputs, m_AssetOutputs, m_SubTxID);
}

const std::vector<Coin::ID> &AssetTxBuilder::GetAssetInputCoins() const
{
    return m_AssetInputCoins;
}

const std::vector<Coin::ID> &AssetTxBuilder::GetAssetOutputCoins() const
{
    return m_AssetOutputCoins;
}

ECC::Point::Native AssetTxBuilder::GetPublicExcess() const
{
    Scalar::Native sk;
    GetSK(sk);
    Point::Native pt = Context::get().G * sk;
    // FIXME: 
    pt = -pt;

    return BaseTxBuilder::GetPublicExcess() + pt;
}
 
Transaction::Ptr AssetTxBuilder::CreateTransaction()
{
    assert(m_Kernel);
    // Don't display in log infinite max height
    if (m_Kernel->m_Height.m_Max == MaxHeight)
    {
        LOG_INFO() << m_Tx.GetTxID() << "[" << m_SubTxID << "]"
                   << " Transaction created. Kernel: " << GetKernelIDString()
                   << " min height: " << m_Kernel->m_Height.m_Min;
    }
    else
    {
        LOG_INFO() << m_Tx.GetTxID() << "[" << m_SubTxID << "]"
                   << " Transaction created. Kernel: " << GetKernelIDString()
                   << " min height: " << m_Kernel->m_Height.m_Min
                   << " max height: " << m_Kernel->m_Height.m_Max;
    }

    // create transaction
    auto transaction = make_shared<Transaction>();
    if (m_EmissionKernel != nullptr) {
        transaction->m_vKernels.push_back(move(m_EmissionKernel));
    }
    transaction->m_vKernels.push_back(move(m_Kernel));
    // TODO: add asset input/outputs?
    transaction->m_Offset = m_Offset + m_PeerOffset;
    transaction->m_vInputs = move(m_Inputs);
    transaction->m_vOutputs = move(m_Outputs);
    move(m_PeerInputs.begin(), m_PeerInputs.end(), back_inserter(transaction->m_vInputs));
    move(m_PeerOutputs.begin(), m_PeerOutputs.end(), back_inserter(transaction->m_vOutputs));

    transaction->Normalize();

    return transaction;
}

void AssetTxBuilder::GetSK(ECC::Scalar::Native &sk) const
{
    auto idx = m_Tx.GetMandatoryParameter<uint64_t>(TxParameterID::AssetKIDIndex, m_SubTxID);
    auto kid = Key::ID(idx, Key::Type::Regular);

    LOG_INFO() << "kid: " << kid.m_Idx;
    // FIXME: is the same kdf?
    m_Tx.GetWalletDB()->get_MasterKdf()->DeriveKey(sk, kid);
}

void AssetTxBuilder::CreateKernel()
{
    BaseTxBuilder::CreateKernel();
    assert(m_Kernel);
    // FIXME: embed it into asset kernel
    // m_Kernel->m_CanEmbed = true;
    // // create kernel for asset
    // m_AssetKernel = make_unique<TxKernel>();
    // m_AssetKernel->m_Fee = 0;
    // m_AssetKernel->m_Height.m_Min = GetMinHeight();
    // m_AssetKernel->m_Height.m_Max = GetMaxHeight();
    // m_AssetKernel->m_Commitment = Zero;

    // m_Tx.SetParameter(TxParameterID::MaxHeight, GetMaxHeight(), m_SubTxID);

    // // FIXME: learn the meaning of lock images
    // // load kernel's extra data
    // Hash::Value peerLockImage;
    // if (m_Tx.GetParameter(TxParameterID::PeerLockImage, peerLockImage, m_SubTxID))
    // {
    //     m_PeerLockImage = make_unique<Hash::Value>(move(peerLockImage));
    // }

    // uintBig preImage;
    // if (m_Tx.GetParameter(TxParameterID::PreImage, preImage, m_SubTxID))
    // {
    //     m_AssetKernel->m_pHashLock = make_unique<TxKernel::HashLock>();
    //     m_AssetKernel->m_pHashLock->m_Preimage = move(preImage);
    // }

    // m_AssetKernel->m_CanEmbed = false;
    // // FIXME: move and swap usage?
    // m_AssetKernel->m_vNested.push_back(std::move(m_Kernel)); 
    Amount assetAmount = GetAssetAmount();
    // TODO: only for issue
    if (assetAmount > 0) {
        m_EmissionKernel = make_unique<TxKernel>();
        m_EmissionKernel->m_AssetEmission = assetAmount; // if amount is negative, will burn token
        m_EmissionKernel->m_Commitment.m_X = m_AssetID;
        m_EmissionKernel->m_Commitment.m_Y = 0;
        // TODO: avoid leak
        
        Scalar::Native sk;
        GetSK(sk);
        m_EmissionKernel->Sign(sk);

        // lstTrg.push_back(std::move(pKrnEmission));
        // // FIXME: 这里不区分emit或burn？
        // skAsset = -skAsset;
        // m_pPeers[0].m_k += skAsset;
    }
    // assert(m_Kernel == nullptr);
    // // FIXME: 
    // m_Kernel.swap(m_AssetKernel);
    // assert(m_AssetKernel == nullptr);
    // assert(m_Kernel != nullptr);
}

void AssetTxBuilder::SignPartial()
{
    // TODO:
    return BaseTxBuilder::SignPartial();
}

} // namespace beam::wallet
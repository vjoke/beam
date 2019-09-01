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
    : BaseTxBuilder(tx, subTxID, {}, fee), 
    m_AssetAmountList{assetAmountList}, m_AssetID{assetID}, m_AssetChange{0}, m_AssetCommand{AssetCommand::Zero},
    m_IssuedBlindingFactor{Zero}
{
    if (m_AssetAmountList.empty())
    {
        m_Tx.GetParameter(TxParameterID::AssetAmountList, m_AssetAmountList, m_SubTxID);
    }

    m_AssetCommand = m_Tx.GetMandatoryParameter<AssetCommand>(TxParameterID::AssetCommand, m_SubTxID);
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

    switch (m_AssetCommand) {
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
            LOG_ERROR() << m_Tx.GetTxID() << " invalid asset command: " << int(m_AssetCommand);
            throw TransactionFailedException(true, TxFailureReason::InvalidTransaction);
        }
        break;

    }
}

void AssetTxBuilder::AddChange()
{
    LOG_INFO() << "AssetTxBuilder::AddChange with change: " << m_Change << " assetChange: " << m_AssetChange;
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
    m_OutputCoins.push_back(Asset{ newUtxo.m_ID, newUtxo.m_assetID });
    // FIXME: 
    m_Tx.SetParameter(TxParameterID::OutputCoins, m_OutputCoins, false, m_SubTxID);
    for (const auto &coin : m_OutputCoins)
    {
        LOG_INFO() << "AssetTxBuilder::GenerateNewCoin result " << coin.m_AssetID;
    }

    if (AssetCommand::Issue == m_AssetCommand && m_IssuedBlindingFactor == Zero)
    {
        SwitchCommitment(&m_AssetID).Create(m_IssuedBlindingFactor, *m_Tx.GetWalletDB()->get_ChildKdf(newUtxo.m_ID), newUtxo.m_ID);
    }
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
    BaseTxBuilder::GenerateOffset();
}

bool AssetTxBuilder::CreateOutputs()
{
    return BaseTxBuilder::CreateOutputs();
}

bool AssetTxBuilder::FinalizeAssetOutputs()
{
    // m_Tx.SetParameter(TxParameterID::AssetOutputs, m_AssetOutputs, false, m_SubTxID);

    // TODO: check transaction size here

    return true;
}

bool AssetTxBuilder::CreateInputs()
{
    return BaseTxBuilder::CreateInputs();
}

void AssetTxBuilder::FinalizeAssetInputs()
{
    // m_Tx.SetParameter(TxParameterID::AssetInputs, m_AssetInputs, false, m_SubTxID);
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
    // PublicExcess = Sum(inputs) - Sum(outputs) - offset * G - (Sum(input amounts) - Sum(output amounts)) * H
    Point::Native publicAmount = Zero;
    Amount amount = 0;
    // TODO: what if user create new asset coin in general tranfer tx?
    for (const auto &cid : m_InputCoins)
    {
        if (cid.m_AssetID == beam::Zero)
        {
            amount += cid.m_IDV.m_Value;
        }
    }
    AmountBig::AddTo(publicAmount, amount);
    amount = 0;
    publicAmount = -publicAmount;
    for (const auto &cid : m_OutputCoins)
    {
        if (cid.m_AssetID == Zero)
        {
            amount += cid.m_IDV.m_Value;
        }
    }
    AmountBig::AddTo(publicAmount, amount);

    Point::Native publicExcess = Context::get().G * m_Offset;
    {
        Point::Native commitment;

        for (const auto &output : m_Outputs)
        {
            // TODO: filter out asset
            if (output->m_AssetID == Zero) 
            {
                if (commitment.Import(output->m_Commitment))
                {
                    publicExcess += commitment;
                }
            }
            
        }

        publicExcess = -publicExcess;
        for (const auto &input : m_Inputs)
        {
            if (commitment.Import(input->m_Commitment))
            {
                publicExcess += commitment;
            }
        }
    }
    publicExcess += publicAmount;

    if (AssetCommand::Issue == m_AssetCommand)
    {
        Point::Native pt = Context::get().G * m_IssuedBlindingFactor;
        // FIXME:
        pt = -pt;

        Scalar::Native sk;
        GetSK(sk);
        Point::Native pt2 = Context::get().G * sk;
        pt2 = -pt2;

        pt += pt2;

        LOG_INFO() << "Ray ==> AssetTxBuilder::GetPublicExcess pt2: " << pt2;
        LOG_INFO() << "Ray ==> AssetTxBuilder::GetPublicExcess blinding factor: " << m_IssuedBlindingFactor;
        publicExcess += pt;
    }

    return publicExcess;
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
        LOG_INFO() << "Emission Kernel commitment " << m_EmissionKernel->m_Commitment;
        transaction->m_vKernels.push_back(move(m_EmissionKernel));
    }
    LOG_INFO() << "Kernel commitment " << m_Kernel->m_Commitment;
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
    Amount assetAmount = GetAssetAmount();
    if (AssetCommand::Issue == m_AssetCommand && assetAmount > 0) {
        m_EmissionKernel = make_unique<TxKernel>();
        m_EmissionKernel->m_AssetEmission = assetAmount; // if amount is negative, will burn token
        m_EmissionKernel->m_Commitment.m_X = m_AssetID;
        m_EmissionKernel->m_Commitment.m_Y = 0;
        // TODO: avoid leak
        Scalar::Native sk;
        GetSK(sk);
        m_EmissionKernel->Sign(sk);

        LOG_INFO() << "Created emission kernel with amount " << assetAmount << " for asset id " << m_AssetID;
        // lstTrg.push_back(std::move(pKrnEmission));
        // skAsset = -skAsset;
        // m_pPeers[0].m_k += skAsset;
    }
}

void AssetTxBuilder::SignPartial()
{
    // create signature
    Point::Native totalPublicExcess = GetPublicExcess();
    totalPublicExcess += m_PeerPublicExcess;
    LOG_INFO() << "Ray ==> AssetTxBuilder::SignPartial " << m_PeerPublicExcess;
    m_Kernel->m_Commitment = totalPublicExcess;

    m_Kernel->get_Hash(m_Message, m_PeerLockImage.get());
    
    auto offset = m_Offset; 
    if (AssetCommand::Issue == m_AssetCommand) 
    {
        Scalar::Native sk;
        GetSK(sk);
        // for issue token only
        offset += sk;
    }
    m_PartialSignature = m_Tx.GetKeyKeeper()->SignSync(m_InputCoins, m_OutputCoins, offset, m_NonceSlot, m_Message, GetPublicNonce() + m_PeerPublicNonce, totalPublicExcess);

    StoreKernelID();
}

} // namespace beam::wallet
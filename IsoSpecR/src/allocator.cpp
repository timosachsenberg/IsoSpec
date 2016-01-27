/*
 *   Copyright (C) 2015 Mateusz Łącki and Michał Startek.
 *
 *   This file is part of IsoSpec.
 *
 *   IsoSpec is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Affero General Public License
 *   version 3, as published by the Free Software Foundation.
 *
 *   IsoSpec is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Affero General Public License for more details.
 *
 *   You should have received a copy of the GNU Affero General Public License
 *   along with IsoSpec.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include "allocator.hpp"



template <typename T>
Allocator<T>::Allocator(const int dim, const int tabSize): currentId(0), dim(dim), tabSize(tabSize)
{
    currentTab = new T[dim * tabSize];
};

template <typename T>
Allocator<T>::~Allocator()
{
    for(unsigned int i = 0; i < prevTabs.size(); ++i)
    {
        delete [] prevTabs[i];
    }

    delete [] currentTab;
};

template <typename T>
void Allocator<T>::shiftTables()
{
    prevTabs.push_back(currentTab);
    currentTab      = new T[dim * tabSize];
    currentId       = 0;
};

template class Allocator<int>;